/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "core/future.hh"
#include "core/iostream.hh"
#include "sstables/exceptions.hh"
#include "sstables/progress_monitor.hh"
#include <seastar/core/byteorder.hh>
#include <seastar/util/variant_utils.hh>

template<typename T>
static inline T consume_be(temporary_buffer<char>& p) {
    T i = read_be<T>(p.get());
    p.trim_front(sizeof(T));
    return i;
}

namespace data_consumer {
enum class proceed { no, yes };
using processing_result = boost::variant<proceed, skip_bytes>;

inline bool operator==(const processing_result& result, proceed value) {
    const proceed* p = boost::get<proceed>(&result);
    return (p != nullptr && *p == value);
}

inline bool operator!=(const processing_result& result, proceed value) {
    return !(result == value);
}

template <typename StateProcessor>
class continuous_data_consumer {
    using proceed = data_consumer::proceed;
    StateProcessor& state_processor() {
        return static_cast<StateProcessor&>(*this);
    };
protected:
    input_stream<char> _input;
    sstables::reader_position_tracker _stream_position;
    // remaining length of input to read (if <0, continue until end of file).
    uint64_t _remain;

    // state machine progress:
    enum class prestate {
        NONE,
        READING_U8,
        READING_U16,
        READING_U32,
        READING_U64,
        READING_BYTES,
    } _prestate = prestate::NONE;

    // state for non-NONE prestates
    uint32_t _pos;
    // state for READING_U8, READING_U16, READING_U32, READING_U64 prestate
    uint8_t  _u8;
    uint16_t _u16;
    uint32_t _u32;
    uint64_t _u64;
    union {
        char bytes[sizeof(uint64_t)];
        uint64_t uint64;
        uint32_t uint32;
        uint16_t uint16;
        uint8_t  uint8;
    } _read_int;
    // state for READING_BYTES prestate
    temporary_buffer<char> _read_bytes;
    temporary_buffer<char>* _read_bytes_where; // which temporary_buffer to set, _key or _val?

    enum class read_status { ready, waiting };
    inline read_status read_8(temporary_buffer<char>& data) {
        if (data.size() >= sizeof(uint8_t)) {
            _u8 = consume_be<uint8_t>(data);
            return read_status::ready;
        } else {
            _pos = 0;
            _prestate = prestate::READING_U8;
            return read_status::waiting;
        }
    }
    // Read a 16-bit integer into _u16. If the whole thing is in the buffer
    // (this is the common case), do this immediately. Otherwise, remember
    // what we have in the buffer, and remember to continue later by using
    // a "prestate":
    inline read_status read_16(temporary_buffer<char>& data) {
        if (data.size() >= sizeof(uint16_t)) {
            _u16 = consume_be<uint16_t>(data);
            return read_status::ready;
        } else {
            std::copy(data.begin(), data.end(), _read_int.bytes);
            _pos = data.size();
            data.trim(0);
            _prestate = prestate::READING_U16;
            return read_status::waiting;
        }
    }
    inline read_status read_32(temporary_buffer<char>& data) {
        if (data.size() >= sizeof(uint32_t)) {
            _u32 = consume_be<uint32_t>(data);
            return read_status::ready;
        } else {
            std::copy(data.begin(), data.end(), _read_int.bytes);
            _pos = data.size();
            data.trim(0);
            _prestate = prestate::READING_U32;
            return read_status::waiting;
        }
    }
    inline read_status read_64(temporary_buffer<char>& data) {
        if (data.size() >= sizeof(uint64_t)) {
            _u64 = consume_be<uint64_t>(data);
            return read_status::ready;
        } else {
            std::copy(data.begin(), data.end(), _read_int.bytes);
            _pos = data.size();
            data.trim(0);
            _prestate = prestate::READING_U64;
            return read_status::waiting;
        }
    }
    inline read_status read_bytes(temporary_buffer<char>& data, uint32_t len, temporary_buffer<char>& where) {
        if (data.size() >=  len) {
            where = data.share(0, len);
            data.trim_front(len);
            return read_status::ready;
        } else {
            // copy what we have so far, read the rest later
            _read_bytes = temporary_buffer<char>(len);
            std::copy(data.begin(), data.end(),_read_bytes.get_write());
            _read_bytes_where = &where;
            _pos = data.size();
            data.trim(0);
            _prestate = prestate::READING_BYTES;
            return read_status::waiting;
        }
    }

    inline void process_buffer(temporary_buffer<char>& data) {
        if (__builtin_expect((_prestate != prestate::NONE), 0)) {
            do_process_buffer(data);
        }
    }
private:
    // This is separated so that the compiler can inline "process_buffer". Because this chunk is too big,
    // it usually won't if this is part of the main function
    void do_process_buffer(temporary_buffer<char>& data) {
        // We're in the middle of reading a basic type, which crossed
        // an input buffer. Resume that read before continuing to
        // handle the current state:
        if (_prestate == prestate::READING_BYTES) {
            auto n = std::min(_read_bytes.size() - _pos, data.size());
            std::copy(data.begin(), data.begin() + n,
                    _read_bytes.get_write() + _pos);
            data.trim_front(n);
            _pos += n;
            if (_pos == _read_bytes.size()) {
                *_read_bytes_where = std::move(_read_bytes);
                _prestate = prestate::NONE;
            }
        } else {
            // in the middle of reading an integer
            unsigned len;
            switch (_prestate) {
            case prestate::READING_U8:
                len = sizeof(uint8_t);
                break;
            case prestate::READING_U16:
                len = sizeof(uint16_t);
                break;
            case prestate::READING_U32:
                len = sizeof(uint32_t);
                break;
            case prestate::READING_U64:
                len = sizeof(uint64_t);
                break;
            default:
                throw sstables::malformed_sstable_exception("unknown prestate");
            }
            assert(_pos < len);
            auto n = std::min((size_t)(len - _pos), data.size());
            std::copy(data.begin(), data.begin() + n, _read_int.bytes + _pos);
            data.trim_front(n);
            _pos += n;
            if (_pos == len) {
                // done reading the integer, store it in _u8, _u16, _u32 or _u64:
                switch (_prestate) {
                case prestate::READING_U8:
                    _u8 = _read_int.uint8;
                    break;
                case prestate::READING_U16:
                    _u16 = net::ntoh(_read_int.uint16);
                    break;
                case prestate::READING_U32:
                    _u32 = net::ntoh(_read_int.uint32);
                    break;
                case prestate::READING_U64:
                    _u64 = net::ntoh(_read_int.uint64);
                    break;
                default:
                    throw sstables::malformed_sstable_exception(
                            "unknown prestate");
                }
                _prestate = prestate::NONE;
            }
        }
    }

    void verify_end_state() {
        state_processor().verify_end_state();
    }
public:
    continuous_data_consumer(input_stream<char>&& input, uint64_t start, uint64_t maxlen)
            : _input(std::move(input)), _stream_position(sstables::reader_position_tracker{start, maxlen}), _remain(maxlen) {}

    future<> consume_input() {
        return _input.consume(state_processor());
    }

    // some states do not consume input (its only exists to perform some
    // action when finishing to read a primitive type via a prestate, in
    // the rare case that a primitive type crossed a buffer). Such
    // non-consuming states need to run even if the data buffer is empty.
    bool non_consuming() {
        return state_processor().non_consuming();
    }

    using unconsumed_remainder = input_stream<char>::unconsumed_remainder;
    using consumption_result_type = consumption_result<char>;

    inline processing_result process(temporary_buffer<char>& data) {
        while (data || non_consuming()) {
            process_buffer(data);
            // If _prestate is set to something other than prestate::NONE
            // after process_buffer was called, it means that data wasn't
            // enough to complete the prestate. That can happen specially
            // when reading a large buf. Thefore, we need to ask caller
            // to read more data until prestate is completed.
            if (__builtin_expect((_prestate != prestate::NONE), 0)) {
                // assert that data was all consumed by process_buffer.
                assert(data.size() == 0);
                return proceed::yes;
            }
            auto ret = state_processor().process_state(data);
            if (__builtin_expect(ret != proceed::yes, 0)) {
                return ret;
            }
        }
        return proceed::yes;
    }

    // called by input_stream::consume():
    future<consumption_result_type>
    operator()(temporary_buffer<char> data) {
        if (data.size() >= _remain) {
            // We received more data than we actually care about, so process
            // the beginning of the buffer, and return the rest to the stream
            auto segment = data.share(0, _remain);
            auto ret = process(segment);
            data.trim_front(_remain - segment.size());
            auto len = _remain - segment.size();
            _remain -= len;
            _stream_position.position += len;
            if (_remain == 0 && ret == proceed::yes) {
                verify_end_state();
            }
            return make_ready_future<consumption_result_type>(stop_consuming<char>{std::move(data)});
        } else if (data.empty()) {
            // End of file
            verify_end_state();
            return make_ready_future<consumption_result_type>(stop_consuming<char>{std::move(data)});
        } else {
            // We can process the entire buffer (if the consumer wants to).
            auto orig_data_size = data.size();
            _stream_position.position += data.size();
            auto result = process(data);
            return visit(result, [this, &data, orig_data_size] (proceed value) {
                _remain -= orig_data_size - data.size();
                _stream_position.position -= data.size();
                if (value == proceed::yes) {
                    return make_ready_future<consumption_result_type>(continue_consuming{});
                } else {
                    return make_ready_future<consumption_result_type>(stop_consuming<char>{std::move(data)});
                }
            }, [this, &data, orig_data_size](skip_bytes skip) {
                // we only expect skip_bytes to be used if reader needs to skip beyond the provided buffer
                // otherwise it should just trim_front and proceed as usual
                assert(data.size() == 0);
                _remain -= orig_data_size;
                if (skip.get_value() >= _remain) {
                    _stream_position.position += _remain;
                    _remain = 0;
                    verify_end_state();
                    return make_ready_future<consumption_result_type>(stop_consuming<char>{std::move(data)});
                }
                _stream_position.position += skip.get_value();
                _remain -= skip.get_value();
                return make_ready_future<consumption_result_type>(std::move(skip));
            });
        }
    }

    future<> fast_forward_to(size_t begin, size_t end) {
        assert(begin >= _stream_position.position);
        auto n = begin - _stream_position.position;
        _stream_position.position = begin;

        assert(end >= _stream_position.position);
        _remain = end - _stream_position.position;

        _prestate = prestate::NONE;
        return _input.skip(n);
    }

    future<> skip_to(size_t begin) {
        return fast_forward_to(begin, _stream_position.position + _remain);
    }

    uint64_t position() const {
        return _stream_position.position;
    }

    const sstables::reader_position_tracker& reader_position() {
        return _stream_position;
    }

    bool eof() const {
        return _remain == 0;
    }

    future<> close() {
        return _input.close();
    }
};
}
