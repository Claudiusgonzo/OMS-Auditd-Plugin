/*
    microsoft-oms-auditd-plugin

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <sys/time.h>
#include "RawEventAccumulator.h"
#include "Translate.h"
#include "Logger.h"

bool RawEvent::AddRecord(std::unique_ptr<RawEventRecord> record) {
    auto rtype = record->GetRecordType();

    if (rtype == RecordType::EOE) {
        return true;
    }

    if (rtype == RecordType::EXECVE) {
        _num_execve_records++;
        if (_num_execve_records == 1) {
            _size += record->GetSize();
            _execve_size += record->GetSize();
            _records.emplace_back(std::move(record));
        } else {
            if (record->GetSize()+_size > MAX_EVENT_SIZE || record->GetSize()+_execve_size > MAX_EXECVE_ACCUM_SIZE || _num_execve_records > MAX_NUM_EXECVE_RECORDS) {
                _num_dropped_records++;
                _drop_count[rtype]++;
                size_t idx = 0;
                if (_execve_records.size() > NUM_EXECVE_RH_PRESERVE) {
                    idx = _execve_records.size() - NUM_EXECVE_RH_PRESERVE - 1;
                }
                _size-=_execve_records[idx]->GetSize();
                _execve_size-=_execve_records[idx]->GetSize();
                _execve_records.erase(_execve_records.begin()+idx);
            }
            _size += record->GetSize();
            _execve_size += record->GetSize();
            _execve_records.emplace_back(std::move(record));
        }
        return false;
    }

    if (record->GetSize()+_size > MAX_EVENT_SIZE || _num_execve_records > MAX_NUM_EXECVE_RECORDS) {
        _num_dropped_records++;
        _drop_count[rtype]++;
    } else {
        _size += record->GetSize();
        _records.emplace_back(std::move(record));
        if (rtype == RecordType::SYSCALL && _syscall_rec_idx < 0) {
            _syscall_rec_idx = _records.size()-1;
        }
    }

    return IsSingleRecordEvent(rtype);
}

int RawEvent::AddEvent(EventBuilder& builder) {
    if (_records.empty() && _num_dropped_records == 0) {
        return 1;
    }
    uint16_t num_records = static_cast<uint16_t>(_records.size()+_execve_records.size());
    if (_num_dropped_records > 0 && _drop_count.size() > 0) {
        num_records += 1;
    }
    auto ret = builder.BeginEvent(_event_id.Seconds(), _event_id.Milliseconds(), _event_id.Serial(), num_records);
    if (ret != 1) {
        return ret;
    }

    if (_syscall_rec_idx > -1) {
        ret = _records[_syscall_rec_idx]->AddRecord(builder);
        if (ret != 1) {
            builder.CancelEvent();
            return ret;
        }
        _records[_syscall_rec_idx].reset(nullptr);
    }

    for (std::unique_ptr<RawEventRecord>& rec: _records) {
        if (!rec) {
            continue;
        }
        ret = rec->AddRecord(builder);
        if (ret != 1) {
            builder.CancelEvent();
            return ret;
        }
        if (rec->GetRecordType() == RecordType::EXECVE) {
            for (std::unique_ptr<RawEventRecord>& rec: _execve_records) {
                ret = rec->AddRecord(builder);
                if (ret != 1) {
                    builder.CancelEvent();
                    return ret;
                }
            }
        }
    }
    if (_num_dropped_records > 0 && _drop_count.size() > 0) {
        ret = builder.BeginRecord(static_cast<uint32_t>(RecordType::AUOMS_DROPPED_RECORDS), std::string_view(RecordTypeToName(RecordType::AUOMS_DROPPED_RECORDS)), std::string_view(""), static_cast<uint16_t>(_drop_count.size()));
        if (ret != 1) {
            builder.CancelEvent();
            return ret;
        }
        for (auto& e: _drop_count) {
            ret = builder.AddField(RecordTypeToName(e.first), std::to_string(e.second), "", field_type_t::UNCLASSIFIED);
            if (ret != 1) {
                builder.CancelEvent();
                return ret;
            }
        }
        ret = builder.EndRecord();
        if (ret != 1) {
            builder.CancelEvent();
            return ret;
        }
    }
    return builder.EndEvent();
}

int RawEventAccumulator::AddRecord(std::unique_ptr<RawEventRecord> record) {
    std::lock_guard<std::mutex> lock(_mutex);

    _bytes_metric->Add(static_cast<double>(record->GetSize()));
    _record_metric->Add(1.0);

    // Drop empty records unless it is the EOE record.
    if (record->IsEmpty() && record->GetRecordType() != RecordType::EOE) {
        return 0;
    }

    auto event_id = record->GetEventId();
    int ret = 0;
    auto found = _events.on(event_id, [this,&record,&ret](size_t entry_count, const std::chrono::steady_clock::time_point& last_touched, std::shared_ptr<RawEvent>& event) {
        if (event->AddRecord(std::move(record))) {
            ret = event->AddEvent(*_builder);
            return CacheEntryOP::REMOVE;
        } else {
            return CacheEntryOP::TOUCH;
        }
    });
    if (!found) {
        auto event = std::make_shared<RawEvent>(record->GetEventId());
        if (event->AddRecord(std::move(record))) {
            _event_metric->Add(1.0);
            return event->AddEvent(*_builder);
        } else {
            _events.add(event_id, event);
        }
    }
    // Don't wait for Flush to be called, preemptively flush oldest if the cache size limit is exceeded
    _events.for_all_oldest_first([this](size_t entry_count, const std::chrono::steady_clock::time_point& last_touched, const EventId& key, std::shared_ptr<RawEvent>& event) {
        if (entry_count > MAX_CACHE_ENTRY) {
            event->AddEvent(*_builder);
            _event_metric->Add(1.0);
            return CacheEntryOP::REMOVE;
        }
        return CacheEntryOP::STOP;
    });
    return 1;
}

void RawEventAccumulator::Flush(long milliseconds) {
    if (milliseconds > 0) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(_mutex);

        _events.for_all_oldest_first([this,now,milliseconds](size_t entry_count, const std::chrono::steady_clock::time_point& last_touched, const EventId& key, std::shared_ptr<RawEvent>& event) {
            if (entry_count > MAX_CACHE_ENTRY || std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()-last_touched.time_since_epoch()) > std::chrono::milliseconds(milliseconds)) {
                event->AddEvent(*_builder);
                _event_metric->Add(1.0);
                return CacheEntryOP::REMOVE;
            }
            return CacheEntryOP::STOP;
        });
    } else {
        _events.for_all_oldest_first([this](size_t entry_count, const std::chrono::steady_clock::time_point& last_touched, const EventId& key, std::shared_ptr<RawEvent>& event) {
            event->AddEvent(*_builder);
            _event_metric->Add(1.0);
            return CacheEntryOP::REMOVE;
        });
    }
}
