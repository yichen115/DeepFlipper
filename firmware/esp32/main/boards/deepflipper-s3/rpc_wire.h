#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dfwire {
constexpr size_t kMaxJson = 767;
// Storage streams up to eight 255-byte names plus metadata in one PB.Main.
constexpr size_t kMaxMessage = 4096;

inline void Varint(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = value & 127;
        value >>= 7;
        out.push_back(byte | (value ? 128 : 0));
    } while (value);
}

inline bool ReadVarint(const std::vector<uint8_t>& data, size_t& pos, uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift <= 28 && pos < data.size(); shift += 7) {
        uint8_t byte = data[pos++];
        if (shift == 28 && byte > 15)
            return false;
        value |= static_cast<uint32_t>(byte & 127) << shift;
        if (!(byte & 128))
            return true;
    }
    return false;
}

inline void UInt(std::vector<uint8_t>& out, uint32_t tag, uint32_t value) {
    Varint(out, tag << 3);
    Varint(out, value);
}

inline void Bytes(std::vector<uint8_t>& out, uint32_t tag, const std::vector<uint8_t>& value) {
    Varint(out, (tag << 3) | 2);
    Varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

inline void String(std::vector<uint8_t>& out, uint32_t tag, const std::string& value) {
    Bytes(out, tag, std::vector<uint8_t>(value.begin(), value.end()));
}

inline std::vector<uint8_t> NativeRequest(uint32_t id, uint32_t tag,
                                          const std::vector<uint8_t>& payload) {
    if (tag < 4 || payload.size() > kMaxMessage - 16)
        return {};
    std::vector<uint8_t> body{8};
    Varint(body, id);
    Bytes(body, tag, payload);
    std::vector<uint8_t> wire;
    Varint(wire, body.size());
    wire.insert(wire.end(), body.begin(), body.end());
    return wire;
}

inline std::vector<uint8_t> Request(uint32_t id, const std::string& json) {
    if (json.size() > kMaxJson)
        return {};
    std::vector<uint8_t> envelope;
    String(envelope, 1, json);
    return NativeRequest(id, 500, envelope);
}

// Queue the complete software-key lifetime together, so transport loss cannot
// leave a press waiting for a later user-level call. RpcGui also releases held
// software keys when its RPC session closes.
inline std::vector<uint8_t> ShortInput(uint32_t first_id, uint32_t key) {
    if (!first_id || first_id > UINT32_MAX - 2 || key >= 6)
        return {};
    std::vector<uint8_t> wire;
    for (uint32_t type : {0U, 2U, 1U}) {
        std::vector<uint8_t> payload;
        UInt(payload, 1, key);
        UInt(payload, 2, type);
        auto part = NativeRequest(first_id++, 23, payload);
        wire.insert(wire.end(), part.begin(), part.end());
    }
    return wire;
}

struct Field {
    uint32_t tag = 0, wire = 0;
    uint64_t number = 0;
    std::vector<uint8_t> bytes;
    std::string text() const { return std::string(bytes.begin(), bytes.end()); }
};

class Reader {
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
    bool valid_ = true;

public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
    bool Next(Field& out) {
        if (!valid_ || pos_ == data_.size())
            return false;
        out = {};
        uint32_t key = 0, length = 0;
        if (!ReadVarint(data_, pos_, key) || !(key >> 3))
            return valid_ = false;
        out.tag = key >> 3;
        out.wire = key & 7;
        if (out.wire == 0) {
            for (unsigned shift = 0; shift < 70 && pos_ < data_.size(); shift += 7) {
                uint8_t byte = data_[pos_++];
                if (shift == 63 && byte > 1)
                    return valid_ = false;
                out.number |= uint64_t(byte & 127) << shift;
                if (!(byte & 128))
                    return true;
            }
            return valid_ = false;
        }
        if (out.wire == 2) {
            if (!ReadVarint(data_, pos_, length) || length > data_.size() - pos_)
                return valid_ = false;
            out.bytes.assign(data_.begin() + pos_, data_.begin() + pos_ + length);
            pos_ += length;
            return true;
        }
        size_t length_fixed = out.wire == 1 ? 8 : out.wire == 5 ? 4 : 0;
        if (!length_fixed || length_fixed > data_.size() - pos_)
            return valid_ = false;
        pos_ += length_fixed;
        return true;
    }
    bool Done() const { return valid_ && pos_ == data_.size(); }
};

struct Message {
    uint32_t id = 0, status = 0, tag = 0;
    bool more = false;
    std::vector<uint8_t> payload;
};

inline bool Decode(const std::vector<uint8_t>& body, Message& message) {
    if (body.size() > kMaxMessage)
        return false;
    message = {};
    Reader reader(body);
    Field field;
    while (reader.Next(field)) {
        if (field.tag <= 3) {
            if (field.wire != 0 || field.number > UINT32_MAX)
                return false;
            if (field.tag == 1)
                message.id = field.number;
            if (field.tag == 2)
                message.status = field.number;
            if (field.tag == 3)
                message.more = field.number != 0;
        } else if (field.wire == 2) {
            if (message.tag)
                return false;
            message.tag = field.tag;
            message.payload = std::move(field.bytes);
        }
    }
    return reader.Done() && (message.tag || message.status);
}

enum class NextResult { NeedMore, Ready, Invalid };
inline NextResult Next(std::vector<uint8_t>& stream, Message& message) {
    size_t prefix = 0;
    uint32_t length = 0;
    if (!ReadVarint(stream, prefix, length))
        return stream.size() >= 5 ? NextResult::Invalid : NextResult::NeedMore;
    if (!length || length > kMaxMessage)
        return NextResult::Invalid;
    if (stream.size() < prefix + length)
        return NextResult::NeedMore;
    std::vector<uint8_t> body(stream.begin() + prefix, stream.begin() + prefix + length);
    if (!Decode(body, message))
        return NextResult::Invalid;
    stream.erase(stream.begin(), stream.begin() + prefix + length);
    return NextResult::Ready;
}

inline bool Envelope(const Message& message, std::string& json) {
    json.clear();
    if (message.tag != 501 || message.status)
        return false;
    Reader reader(message.payload);
    Field field;
    if (!reader.Next(field) || field.tag != 1 || field.wire != 2 || field.bytes.size() > kMaxJson ||
        !reader.Done())
        return false;
    json = field.text();
    return !json.empty();
}

inline bool Response(const std::vector<uint8_t>& body, uint32_t& id, std::string& json) {
    Message message;
    if (!Decode(body, message))
        return false;
    id = message.id;
    return Envelope(message, json);
}
}  // namespace dfwire
