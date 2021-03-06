/*
    microsoft-oms-auditd-plugin

    Copyright (c) Microsoft Corporation

    All rights reserved. 

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#ifndef AUOMS_EVENTQUEUE_H
#define AUOMS_EVENTQUEUE_H

#include "Event.h"
#include "Queue.h"

class EventQueue: public IEventBuilderAllocator {
public:
    explicit EventQueue(std::shared_ptr<Queue> queue): _buffer(), _size(0), _queue(std::move(queue)) {}

    int Allocate(void** data, size_t size) override {
        if (_size != size) {
            _size = size;
        }
        if (_buffer.size() < _size) {
            _buffer.resize(_size);
        }
        *data = _buffer.data();
        return 1;
    }

    int Commit() override {
        auto ret =  _queue->Put(_buffer.data(), _size);
        _size = 0;
        return ret;
    }

    int Rollback() override {
        _size = 0;
        return 1;
    }

private:
    std::vector<uint8_t> _buffer;
    size_t _size;
    std::shared_ptr<Queue> _queue;
};


#endif //AUOMS_EVENTQUEUE_H
