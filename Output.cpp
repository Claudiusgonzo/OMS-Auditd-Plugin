/*
    microsoft-oms-auditd-plugin

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "Output.h"
#include "Logger.h"
#include "UnixDomainWriter.h"

#include "OMSEventWriter.h"
#include "JSONEventWriter.h"
#include "MsgPackEventWriter.h"
#include "RawEventWriter.h"
#include "SyslogEventWriter.h"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}


/****************************************************************************
 *
 ****************************************************************************/

AckQueue::AckQueue(size_t max_size): _max_size(max_size), _closed(false), _have_auto_cursor(false), _next_seq(0), _auto_cursor_seq(0) {}

void AckQueue::Close() {
    std::unique_lock<std::mutex> _lock(_mutex);
    _closed = true;
    _cond.notify_all();
}

bool AckQueue::Add(const EventId& event_id, const QueueCursor& cursor, long timeout) {
    std::unique_lock<std::mutex> _lock(_mutex);

    if (_cond.wait_for(_lock, std::chrono::milliseconds(timeout), [this]() { return _closed || _event_ids.size() < _max_size; })) {
        auto seq = _next_seq++;
        _event_ids.emplace(event_id, seq);
        _cursors.emplace(seq, std::make_pair(event_id, cursor));
        return true;
    }
    return false;
}

void AckQueue::SetAutoCursor(const QueueCursor& cursor) {
    std::unique_lock<std::mutex> _lock(_mutex);

    _auto_cursor_seq = _next_seq++;
    _auto_cursor = cursor;
    _have_auto_cursor = true;
}

bool AckQueue::GetAutoCursor(QueueCursor& cursor) {
    std::unique_lock<std::mutex> _lock(_mutex);

    if (_have_auto_cursor) {
        cursor = _auto_cursor;
        _have_auto_cursor = false;
        return true;
    }
    return false;
}

void AckQueue::Remove(const EventId& event_id) {
    std::unique_lock<std::mutex> _lock(_mutex);

    auto eitr = _event_ids.find(event_id);
    if (eitr == _event_ids.end()) {
        return;
    }
    auto seq = eitr->second;
    _event_ids.erase(eitr);

    _cursors.erase(seq);
}

void AckQueue::Reset() {
    std::unique_lock<std::mutex> _lock(_mutex);

    _closed = false;
    _event_ids.clear();
    _cursors.clear();
    _next_seq = 0;
    _have_auto_cursor = false;
    _auto_cursor_seq = 0;
}

bool AckQueue::Wait(int millis) {
    std::unique_lock<std::mutex> _lock(_mutex);

    auto now = std::chrono::steady_clock::now();
    return _cond.wait_until(_lock, now + std::chrono::milliseconds(millis), [this] { return _event_ids.empty(); });
}

bool AckQueue::Ack(const EventId& event_id, QueueCursor& cursor) {
    std::unique_lock<std::mutex> _lock(_mutex);

    bool found = false;
    uint64_t seq = 0;

    auto eitr = _event_ids.find(event_id);
    if (eitr != _event_ids.end()) {
        seq = eitr->second;
        _event_ids.erase(eitr);
        _cond.notify_all(); // _event_ids was modified, so notify any waiting Add calls

        // Find and remove all from cursors that are <= seq
        while (!_cursors.empty() && _cursors.begin()->first <= seq) {
            cursor = _cursors.begin()->second.second;
            found = true;
            // Make sure to remove any associated event ids from _event_ids.
            _event_ids.erase(_cursors.begin()->second.first);
            _cursors.erase(_cursors.begin());
        }
    }

    /*
     * If the auto cursor is present return it instead if:
     *      1) The acked event id isn't present or _auto_cursor_seq is newer than the acked seq
     *      2) and, _cursors is empty or the oldest seq in _cursors is > than _auto_cursor_seq
     */
    if (_have_auto_cursor) {
        if (!found || _auto_cursor_seq > seq) {
            if (_cursors.empty() || _cursors.begin()->first > _auto_cursor_seq) {
                found = true;
                cursor = _auto_cursor;
                _have_auto_cursor = false;
            }
        }
    }

    return found;
}

/****************************************************************************
 *
 ****************************************************************************/

bool CursorWriter::Read() {
    std::lock_guard<std::mutex> lock(_mutex);

    std::array<uint8_t, QueueCursor::DATA_SIZE> data;

    int fd = open(_path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            Logger::Error("Output(%s): Failed to open cursor file (%s): %s", _name.c_str(), _path.c_str(), std::strerror(errno));
            return false;
        } else {
            _cursor = QueueCursor::HEAD;
            return true;
        }
    }

    auto ret = read(fd, data.data(), data.size());
    if (ret != data.size()) {
        if (ret >= 0) {
            Logger::Error("Output(%s): Failed to read cursor file (%s): only %ld bytes out of %ld where read", _name.c_str(), _path.c_str(), ret, data.size());
        } else {
            Logger::Error("Output(%s): Failed to read cursor file (%s): %s", _name.c_str(), _path.c_str(), std::strerror(errno));
        }
        close(fd);
        return false;
    }
    close(fd);

    _cursor.from_data(data);

    return true;
}

bool CursorWriter::Write() {
    std::lock_guard<std::mutex> lock(_mutex);

    std::array<uint8_t, QueueCursor::DATA_SIZE> data;
    _cursor.to_data(data);

    int fd = open(_path.c_str(), O_WRONLY|O_CREAT, 0600);
    if (fd < 0) {
        Logger::Error("Output(%s): Failed to open/create cursor file (%s): %s", _name.c_str(), _path.c_str(), std::strerror(errno));
        return false;
    }

    auto ret = write(fd, data.data(), data.size());
    if (ret != data.size()) {
        if (ret >= 0) {
            Logger::Error("Output(%s): Failed to write cursor file (%s): only %ld bytes out of %ld where written", _name.c_str(), _path.c_str(), ret, data.size());
        } else {
            Logger::Error("Output(%s): Failed to write cursor file (%s): %s", _name.c_str(), _path.c_str(), std::strerror(errno));
        }
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool CursorWriter::Delete() {
    auto ret = unlink(_path.c_str());
    if (ret != 0 && errno != ENOENT) {
        Logger::Error("Output(%s): Failed to delete cursor file (%s): %s", _name.c_str(), _path.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

QueueCursor CursorWriter::GetCursor() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _cursor;
}

void CursorWriter::UpdateCursor(const QueueCursor& cursor) {
    std::lock_guard<std::mutex> lock(_mutex);
    _cursor = cursor;
    _cursor_updated = true;
    _cond.notify_all();
}

void CursorWriter::on_stopping() {
    std::lock_guard<std::mutex> lock(_mutex);
    _cursor_updated = true;
    _cond.notify_all();
}

void CursorWriter::run() {
    while (!IsStopping()) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cond.wait(lock, [this]{ return _cursor_updated;});
            _cursor_updated = false;
        }
        Write();
        _sleep(100);
    }
    Write();
}

/****************************************************************************
 *
 ****************************************************************************/
void AckReader::Init(std::shared_ptr<IEventWriter> event_writer,
                     std::shared_ptr<IOBase> writer,
                     std::shared_ptr<AckQueue> ack_queue,
                     std::shared_ptr<CursorWriter> cursor_writer) {
    _event_writer = event_writer;
    _writer = writer;
    _queue = ack_queue;
    _cursor_writer = cursor_writer;
}

void AckReader::run() {
    EventId id;
    QueueCursor cursor;
    while(_event_writer->ReadAck(id, _writer.get()) == IO::OK) {
        if (_queue->Ack(id, cursor)) {
            _cursor_writer->UpdateCursor(cursor);
        }
    }
    // The connection is lost, Close writer here so that Output::handle_events will exit
    _writer->Close();

    if (_queue->GetAutoCursor(cursor)) {
        // There was still a pending auto cursor, so update the _cursor_writer
        _cursor_writer->UpdateCursor(cursor);
    }

    // Make sure any waiting AckQueue::Add() returns immediately instead of waiting for the timeout.
    _queue->Close();
}

/****************************************************************************
 *
 ****************************************************************************/

std::shared_ptr<IEventWriter> RawOnlyEventWriterFactory::CreateEventWriter(const std::string& name, const Config& config) {
    std::string format = "raw";

    if (config.HasKey("output_format")) {
        format = config.GetString("output_format");
    }

    if (format == "raw") {
        return std::unique_ptr<IEventWriter>(static_cast<IEventWriter*>(new RawEventWriter()));
    } else {
        return nullptr;
    }
}

/****************************************************************************
 *
 ****************************************************************************/

bool Output::IsConfigDifferent(const Config& config) {
    return *_config != config;
}

bool Output::Load(std::unique_ptr<Config>& config) {
    Logger::Info("Output(%s): Loading config", _name.c_str());

    _config = std::unique_ptr<Config>(new(Config));
    *_config = *config;

    std::string format = "oms";

    if (_config->HasKey("output_format")) {
        format = _config->GetString("output_format");
    }

    // For syslog skip the socket check as this writes directly to Syslog
    std::string socket_path = "";

    if (format.compare("syslog")) {
        if (!_config->HasKey("output_socket")) {
            Logger::Error("Output(%s): Missing required parameter: output_socket", _name.c_str());
            return false;
        } 
        socket_path = _config->GetString("output_socket");
    }

    _event_writer = _writer_factory->CreateEventWriter(_name, *_config);
    if (!_event_writer) {
        return false;
    }

    if (_filter_factory) {
        _event_filter = _filter_factory->CreateEventFilter(_name, *_config);
    } else {
        _event_filter.reset();
    }

    if (socket_path != _socket_path || !_writer) {
        _socket_path = socket_path;
        _writer = std::unique_ptr<UnixDomainWriter>(new UnixDomainWriter(_socket_path));
    }

    if (_config->HasKey("enable_ack_mode")) {
        try {
            _ack_mode = _config->GetBool("enable_ack_mode");
        } catch (std::exception) {
            Logger::Error("Output(%s): Invalid enable_ack_mode parameter value", _name.c_str());
            return false;
        }
    }

    if (_ack_mode) {
        uint64_t ack_queue_size = DEFAULT_ACK_QUEUE_SIZE;
        if (_config->HasKey("ack_queue_size")) {
            try {
                ack_queue_size = _config->GetUint64("ack_queue_size");
            } catch (std::exception) {
                Logger::Error("Output(%s): Invalid ack_queue_size parameter value", _name.c_str());
                return false;
            }
        }
        if (ack_queue_size < 1) {
            Logger::Error("Output(%s): Invalid ack_queue_size parameter value", _name.c_str());
            return false;
        }

        if (_config->HasKey("ack_timeout")) {
            try {
                _ack_timeout = _config->GetInt64("ack_timeout");
            } catch (std::exception) {
                Logger::Error("Output(%s): Invalid ack_timeout parameter value", _name.c_str());
                return false;
            }
        }
        if (_ack_timeout == 0 || (_ack_timeout > 0 && _ack_timeout < MIN_ACK_TIMEOUT)) {
            Logger::Warn("Output(%s): ack_timeout parameter value to small (%ld), using (%ld)", _name.c_str(), _ack_timeout, MIN_ACK_TIMEOUT);
            _ack_timeout = MIN_ACK_TIMEOUT;
        }

        if (!_ack_queue || _ack_queue->MaxSize() != ack_queue_size) {
            _ack_queue = std::make_shared<AckQueue>(ack_queue_size);
        }
    } else {
        if (_ack_queue) {
            _ack_queue.reset();
        }
    }
    return true;

}

// Delete any resources associated with the output
void Output::Delete() {
    _cursor_writer->Delete();
    Logger::Info("Output(%s): Removed", _name.c_str());
}

bool Output::check_open()
{
    int sleep_period = START_SLEEP_PERIOD;

    while(!IsStopping()) {
        if (_writer->IsOpen()) {
            return true;
        }
        Logger::Info("Output(%s): Connecting to %s", _name.c_str(), _socket_path.c_str());
        if (_writer->Open()) {
            if (IsStopping()) {
                _writer->Close();
                return false;
            }
            Logger::Info("Output(%s): Connected", _name.c_str());
            return true;
        } else {
            Logger::Warn("Output(%s): Failed to connect to '%s': %s", _name.c_str(), _socket_path.c_str(), std::strerror(errno));
        }

        Logger::Info("Output(%s): Sleeping %d seconds before re-trying connection", _name.c_str(), sleep_period);

        if (_sleep(sleep_period*1000)) {
            return false;
        }

        sleep_period = sleep_period * 2;
        if (sleep_period > MAX_SLEEP_PERIOD) {
            sleep_period = MAX_SLEEP_PERIOD;
        }
    }
    return false;
}

bool Output::handle_events(bool checkOpen) {
    std::array<uint8_t, Queue::MAX_ITEM_SIZE> data;

    _cursor = _cursor_writer->GetCursor();
    _cursor_writer->Start();

    if (_ack_mode) {
        _ack_reader->Init(_event_writer, _writer, _ack_queue, _cursor_writer);
        _ack_reader->Start();
        _ack_queue->Reset();
    }

    while(!IsStopping() && (!checkOpen || _writer->IsOpen())) {
        QueueCursor cursor;
        size_t size = data.size();

        int ret;
        do {
            ret = _queue->Get(_cursor, data.data(), &size, &cursor, 100);
        } while(ret == Queue::TIMEOUT && (!checkOpen || _writer->IsOpen()));

        if (ret == Queue::INTERRUPTED) {
            continue;
        }

        if (ret == Queue::BUFFER_TOO_SMALL) {
            Logger::Error("Output(%s): Encountered possible corruption in queue, resetting queue", _name.c_str());
            _queue->Reset();
            break;
        }

        if (ret == Queue::OK && (!checkOpen || _writer->IsOpen()) && !IsStopping()) {
            auto vs = Event::GetVersionAndSize(data.data());
            if (vs.second != size) {
                Logger::Error("Output(%s): Encountered possible corruption in queue, resetting queue", _name.c_str());
                _queue->Reset();
                break;
            }

            Event event(data.data(), size);
            bool filtered = _event_filter && _event_filter->IsEventFiltered(event);
            if (!filtered) {
                if (_ack_mode) {
                    // Avoid racing with receiver, add ack before sending event
                    if (!_ack_queue->Add(EventId(event.Seconds(), event.Milliseconds(), event.Serial()), cursor,
                                         _ack_timeout)) {
                        if (_writer->IsOpen()) {
                            Logger::Error("Output(%s): Timeout waiting for Acks", _name.c_str());
                        }
                        break;
                    }
                }

                auto ret = _event_writer->WriteEvent(event, _writer.get());
                if (ret == IEventWriter::NOOP) {
                    if (_ack_mode) {
                        // The event was not sent, so remove it's ack
                        _ack_queue->Remove(EventId(event.Seconds(), event.Milliseconds(), event.Serial()));
                        // And update the auto cursor
                        _ack_queue->SetAutoCursor(cursor);
                    }
                } else if (ret != IWriter::OK) {
                    break;
                }
                _cursor = cursor;

                if (!_ack_mode) {
                    _cursor_writer->UpdateCursor(cursor);
                }
            } else {
                _cursor = cursor;
                if (_ack_mode) {
                    _ack_queue->SetAutoCursor(cursor);
                } else {
                    _cursor_writer->UpdateCursor(cursor);
                }
            }
        }
    }

    if (_ack_mode) {
        // Wait a short time for final acks to arrive
        _ack_queue->Wait(100);
    }

    // writer must be closed before calling _ack_reader->Stop(), or the stop may hang until the connection is closed remotely.
    _writer->Close();

    if (_ack_mode) {
        _ack_reader->Stop();
    }

    if (!IsStopping()) {
        Logger::Info("Output(%s): Connection lost", _name.c_str());
    }

    _cursor_writer->Stop();

    return !IsStopping();
}

void Output::on_stopping() {
    Logger::Info("Output(%s): Stopping", _name.c_str());
    _queue->Interrupt();
    if (_writer) {
        _writer->CloseWrite();
    }
    if (_ack_queue) {
        _ack_queue->Close();
    }
}

void Output::on_stop() {
    if (_ack_reader) {
        _ack_reader->Stop();
    }
    if (_writer) {
        _writer->Close();
    }
    if (_cursor_writer) {
        _cursor_writer->Stop();
    }
    _cursor_writer->Write();
    Logger::Info("Output(%s): Stopped", _name.c_str());
}

void Output::run() {
    Logger::Info("Output(%s): Started", _name.c_str());

    if (!_cursor_writer->Read()) {
        Logger::Error("Output(%s): Aborting because cursor file is unreadable", _name.c_str());
        return;
    }

    bool checkOpen = true;

    _cursor = _cursor_writer->GetCursor();
     if (!_config->HasKey("output_socket")) {
           checkOpen = false;
    }


    while(!IsStopping()) {
        while (!checkOpen || check_open()) {
            if (!handle_events(checkOpen)) {
                return;
            }
        }
    }
}
