#include "ops.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <charconv>
#include <functional>
#include <limits>

namespace tool {

using ebx::BoxedReference;
using ebx::Document;
using ebx::FieldCategory;
using ebx::FieldDescriptor;
using ebx::FieldType;
using ebx::Object;
using ebx::PointerKind;
using ebx::PointerReference;
using ebx::Value;
using J = rapidjson::Value;

namespace {

std::string json_text(const J& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetSize());
}

// UTF-8 (JSON) -> the Latin-1 bytes EBX strings hold (ASCII passes through).
std::string from_utf8(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { out.push_back(static_cast<char>(c)); continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            const auto cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
            if (cp < 0x100) { out.push_back(static_cast<char>(cp)); ++i; continue; }
        }
        throw std::runtime_error("string has characters outside Latin-1");
    }
    return out;
}

struct Token {
    bool index{};
    std::string name;
    std::size_t at{};
};

std::vector<Token> parse_path(std::string_view path) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < path.size()) {
        const auto c = path[i];
        if (c == '.' || c == ' ') { ++i; continue; }
        if (c == '[') {
            const auto close = path.find(']', i);
            if (close == std::string_view::npos) throw std::runtime_error("unclosed [ in path " + std::string(path));
            Token t;
            t.index = true;
            const auto digits = path.substr(i + 1, close - i - 1);
            const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), t.at);
            if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size())
                throw std::runtime_error("bad index in path " + std::string(path));
            out.push_back(t);
            i = close + 1;
            continue;
        }
        auto end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') ++end;
        Token t;
        t.name = std::string(path.substr(i, end - i));
        out.push_back(t);
        i = end;
    }
    return out;
}

enum class NodeKind { object, structured, raw };

struct Node {
    NodeKind kind{};
    Object* object{};                    // object
    Value* value{};                      // structured
    const FieldDescriptor* field{};
    bool element{};
    std::uint32_t at{};                  // raw
    RawType type;
    std::string where;
};

RawType raw_type_of(const FieldDescriptor& field) {
    RawType out;
    out.type = field.type();
    out.array = field.category() == FieldCategory::array;
    if (field.type() == FieldType::structure || field.type() == FieldType::enumeration) out.typeIndex = field.classRef;
    return out;
}

struct Integer {
    bool negative{};
    std::uint64_t magnitude{};
};

class Applier final {
public:
    Applier(Document& document, EditState& state, ApplyResult& result) : d_(document), st_(state), r_(result) {}

    void run(const std::string& text) {
        rapidjson::Document json;
        json.Parse(text.c_str(), text.size());
        if (json.HasParseError())
            throw std::runtime_error(std::string("ops JSON: ") + rapidjson::GetParseError_En(json.GetParseError()) +
                                     " at offset " + std::to_string(json.GetErrorOffset()));
        const J* ops = &json;
        bool sort = exported_sorted(d_);
        if (json.IsObject()) {
            if (!json.HasMember("ops") || !json["ops"].IsArray()) throw std::runtime_error("ops file needs an \"ops\" array");
            ops = &json["ops"];
            if (json.HasMember("sort") && json["sort"].IsBool()) sort = sort && json["sort"].GetBool();
        }
        if (!ops->IsArray()) throw std::runtime_error("ops file must be a JSON array or {\"ops\": [...]}");
        std::size_t number = 0;
        for (const auto& op : ops->GetArray()) {
            ++number;
            try {
                one(op);
            } catch (const std::exception& error) {
                throw std::runtime_error("op " + std::to_string(number) + " " + json_text(op).substr(0, 300) + ": " + error.what());
            }
        }
        r_.sorted = sort;
        r_.moveTo = order_instances(d_, sort);
        for (const auto& [name, index] : st_.names)
            r_.lines.push_back("$" + name + " = instance " + std::to_string(index) + " before ordering -> " +
                               std::to_string(r_.moveTo.at(index)) + " in the output" +
                               (d_.instances[r_.moveTo.at(index)].exported
                                    ? " (guid " + guid_text(d_.instances[r_.moveTo.at(index)].instanceGuid) + ")" : ""));
    }

private:
    static std::string str(const J& v, const char* what) {
        if (!v.IsString()) throw std::runtime_error(std::string(what) + " must be a string");
        return std::string(v.GetString(), v.GetStringLength());
    }
    static const J* member(const J& op, const char* name) {
        if (!op.IsObject()) return nullptr;
        const auto found = op.FindMember(name);
        return found == op.MemberEnd() ? nullptr : &found->value;
    }
    static const J& need(const J& op, const char* name) {
        const auto* v = member(op, name);
        if (!v) throw std::runtime_error(std::string("missing \"") + name + "\"");
        return *v;
    }

    void one(const J& op) {
        const auto kind = str(need(op, "op"), "op");
        if (kind == "clone_instance") return op_clone(op);
        if (kind == "set") return op_set(op);
        if (kind == "append") return op_append(op);
        if (kind == "add_import") {
            const auto index = ensure_import(d_, parse_guid(str(need(op, "file"), "file")), parse_guid(str(need(op, "instance"), "instance")));
            r_.lines.push_back("add_import -> import " + std::to_string(index));
            return;
        }
        if (kind == "set_file_guid") {
            d_.fileGuid = parse_guid(str(need(op, "guid"), "guid"));
            r_.lines.push_back("set_file_guid -> " + guid_text(d_.fileGuid));
            return;
        }
        if (kind == "set_guid") {
            const auto index = instance(need(op, "instance"));
            auto& inst = d_.instances[index];
            if (!inst.exported) throw std::runtime_error("only an exported instance has a guid");
            const auto guid = parse_guid(str(need(op, "guid"), "guid"));
            for (const auto& other : d_.instances)
                if (&other != &inst && other.exported && other.instanceGuid == guid) throw std::runtime_error("guid already used");
            inst.instanceGuid = guid;
            r_.lines.push_back("set_guid instance " + std::to_string(index) + " -> " + guid_text(guid));
            return;
        }
        throw std::runtime_error("unknown op " + kind);
    }

    // ---- instance selectors ----
    std::size_t instance(const J& selector) {
        if (selector.IsUint64() || selector.IsInt64()) {
            const auto index = selector.IsUint64() ? selector.GetUint64() : static_cast<std::uint64_t>(selector.GetInt64());
            if (index >= d_.instances.size()) throw std::runtime_error("instance " + std::to_string(index) + " does not exist");
            return static_cast<std::size_t>(index);
        }
        if (selector.IsString()) {
            const auto text = str(selector, "instance");
            if (text == "root") return 0;
            if (!text.empty() && text[0] == '$') {
                const auto found = st_.names.find(text.substr(1));
                if (found == st_.names.end()) throw std::runtime_error("no instance named " + text);
                return found->second;
            }
            if (looks_like_guid(text)) {
                const auto guid = parse_guid(text);
                for (std::size_t i = 0; i < d_.instances.size(); ++i)
                    if (d_.instances[i].exported && d_.instances[i].instanceGuid == guid) return i;
                throw std::runtime_error("no exported instance " + text);
            }
            throw std::runtime_error("bad instance selector " + text);
        }
        if (selector.IsObject()) {
            if (const auto* g = member(selector, "guid")) return instance(*g);
            const auto* type = member(selector, "type");
            const auto* where = member(selector, "where");
            std::size_t nth = 0;
            if (const auto* n = member(selector, "nth"); n && n->IsUint()) nth = n->GetUint();
            std::size_t seen = 0;
            for (std::size_t i = 0; i < d_.instances.size(); ++i) {
                const auto& inst = d_.instances[i];
                if (type && lower(type_name(d_, inst.descriptor)) != lower(str(*type, "type"))) continue;
                if (where && (!inst.object || !matches(*inst.object, *where))) continue;
                if (seen++ == nth) return i;
            }
            throw std::runtime_error("no instance matches " + json_text(selector));
        }
        throw std::runtime_error("bad instance selector " + json_text(selector));
    }

    bool matches(const Object& object, const J& where) {
        if (!where.IsObject()) throw std::runtime_error("\"where\" must be an object");
        for (const auto& m : where.GetObject()) {
            const std::string name(m.name.GetString(), m.name.GetStringLength());
            const ebx::FieldValue* field = object.find(name);
            if (!field) return false;
            const auto& v = field->value;
            const auto& want = m.value;
            if (const auto* s = std::get_if<std::string>(&v.data)) {
                if (!want.IsString() || lower(*s) != lower(from_utf8(str(want, "where")))) return false;
            } else if (const auto* b = std::get_if<bool>(&v.data)) {
                if (!want.IsBool() || want.GetBool() != *b) return false;
            } else if (const auto* i = std::get_if<std::int64_t>(&v.data)) {
                if (!want.IsInt64() || want.GetInt64() != *i) return false;
            } else if (const auto* u = std::get_if<std::uint64_t>(&v.data)) {
                if (!want.IsUint64() || want.GetUint64() != *u) return false;
            } else if (const auto* f = std::get_if<double>(&v.data)) {
                if (!want.IsNumber() || want.GetDouble() != *f) return false;
            } else {
                return false;
            }
        }
        return true;
    }

    // ---- paths ----
    Node root(std::size_t index) {
        auto& inst = d_.instances[index];
        if (!inst.object) throw std::runtime_error("instance has no parsed object");
        Node n;
        n.kind = NodeKind::object;
        n.object = inst.object.get();
        n.where = "instance " + std::to_string(index);
        return n;
    }

    Node resolve(std::size_t index, std::string_view path) {
        auto node = root(index);
        for (const auto& token : parse_path(path)) node = step(node, token);
        return node;
    }

    Node resolve_from(Node node, std::string_view path) {
        for (const auto& token : parse_path(path)) node = step(node, token);
        return node;
    }

    static bool is_array_node(const Node& n) {
        return n.kind == NodeKind::structured && !n.element && n.field->category() == FieldCategory::array;
    }

    Node descend_boxed(const Node& n) {
        std::int64_t recordOffset = -1;
        if (n.kind == NodeKind::structured) {
            const auto* b = std::get_if<BoxedReference>(&n.value->data);
            if (!b || b->encodedType == 0) throw std::runtime_error(n.where + " is an empty boxed value");
            recordOffset = b->dataOffset;
        } else {
            if (load<std::uint32_t>(d_.payload, n.at) == 0) throw std::runtime_error(n.where + " is an empty boxed value");
            recordOffset = read_relative(d_, n.at + 8).target;
        }
        const auto record = recordOffset >= 0 ? boxed_record_at(d_, static_cast<std::uint32_t>(recordOffset)) : std::nullopt;
        if (!record) throw std::runtime_error(n.where + " points at no boxed record");
        const auto& rec = d_.boxedValues[*record];
        Node out;
        out.kind = NodeKind::raw;
        out.at = rec.offset;
        out.type = record_type(d_, rec.encodedType, rec.classRef);
        out.where = n.where + "{boxed}";
        return out;
    }

    bool is_boxed(const Node& n) const {
        if (n.kind == NodeKind::structured) return !is_array_node(n) && n.field->type() == FieldType::boxedValueRef;
        if (n.kind == NodeKind::raw) return !n.type.array && n.type.type == FieldType::boxedValueRef;
        return false;
    }

    // raw array slot -> (array record index, element start)
    std::pair<std::size_t, std::uint32_t> raw_array(const Node& n) {
        const auto rel = read_relative(d_, n.at);
        if (rel.sentinel || rel.target < 0) throw std::runtime_error(n.where + " is not a valid array slot");
        const auto record = array_record_at(d_, static_cast<std::uint32_t>(rel.target));
        if (!record) throw std::runtime_error(n.where + " is an empty array without storage (cannot grow in place)");
        return {*record, static_cast<std::uint32_t>(rel.target)};
    }

    Node step(const Node& n, const Token& t) {
        const auto where = n.where + (t.index ? "[" + std::to_string(t.at) + "]" : "." + t.name);
        switch (n.kind) {
        case NodeKind::object: {
            if (t.index) throw std::runtime_error(where + ": an object has no elements");
            for (auto& field : n.object->fields) {
                if (lower(field.name) != lower(t.name)) continue;
                Node out;
                out.kind = NodeKind::structured;
                out.value = &field.value;
                out.field = &d_.fields[field.descriptor];
                out.where = where;
                return out;
            }
            throw std::runtime_error(where + ": no such field (type " + type_name(d_, n.object->descriptor) + ")");
        }
        case NodeKind::structured: {
            if (is_array_node(n)) {
                if (!t.index) throw std::runtime_error(where + ": an array needs [index]");
                auto* items = std::get_if<Value::Array>(&n.value->data);
                if (!items || t.at >= items->size()) throw std::runtime_error(where + ": index out of range");
                Node out = n;
                out.value = &(*items)[t.at];
                out.element = true;
                out.where = where;
                return out;
            }
            if (n.field->type() == FieldType::structure) {
                auto* nested = std::get_if<std::shared_ptr<Object>>(&n.value->data);
                if (!nested || !*nested) throw std::runtime_error(where + ": struct has no value");
                Node object;
                object.kind = NodeKind::object;
                object.object = nested->get();
                object.where = n.where;
                return step(object, t);
            }
            if (n.field->type() == FieldType::boxedValueRef) return step(descend_boxed(n), t);
            if (n.field->type() == FieldType::pointer) {
                const auto* p = std::get_if<PointerReference>(&n.value->data);
                if (!p || p->kind != PointerKind::internal || p->index < 0)
                    throw std::runtime_error(where + ": only an internal pointer can be followed");
                auto object = root(static_cast<std::size_t>(p->index));
                object.where = n.where + "->";
                return step(object, t);
            }
            throw std::runtime_error(where + ": cannot step into a " + field_type_name(n.field->type()));
        }
        case NodeKind::raw: {
            if (n.type.array) {
                if (!t.index) throw std::runtime_error(where + ": an array needs [index]");
                const auto [record, start] = raw_array(n);
                if (t.at >= d_.arrays[record].count) throw std::runtime_error(where + ": index out of range");
                Node out;
                out.kind = NodeKind::raw;
                out.type = n.type;
                out.type.array = false;
                out.at = static_cast<std::uint32_t>(start + t.at * value_size(d_, n.type.type, n.type.typeIndex));
                out.where = where;
                return out;
            }
            if (n.type.type == FieldType::structure) {
                if (t.index) throw std::runtime_error(where + ": a struct has no elements");
                const auto field = find_field(d_, static_cast<std::size_t>(n.type.typeIndex), t.name);
                if (!field) throw std::runtime_error(where + ": no such field (type " + type_name(d_, n.type.typeIndex) + ")");
                const auto& f = d_.fields[*field];
                Node out;
                out.kind = NodeKind::raw;
                out.type = raw_type_of(f);
                out.at = n.at + f.dataOffset;
                out.where = where;
                return out;
            }
            if (n.type.type == FieldType::boxedValueRef) return step(descend_boxed(n), t);
            if (n.type.type == FieldType::pointer) {
                const auto v = load<std::uint64_t>(d_.payload, n.at);
                if (v == 0 || (v & 1U)) throw std::runtime_error(where + ": only an internal pointer can be followed");
                const auto target = classify(d_, static_cast<std::uint32_t>(read_relative(d_, n.at).target));
                if (target.kind != TargetKind::instance) throw std::runtime_error(where + ": the pointer reaches no instance");
                auto object = root(target.index);
                object.where = n.where + "->";
                return step(object, t);
            }
            throw std::runtime_error(where + ": cannot step into a " + field_type_name(n.type.type));
        }
        }
        throw std::runtime_error("bad node");
    }

    // ---- values ----
    static Integer integer(const J& v) {
        if (v.IsObject()) {
            if (const auto* text = member(v, "djb2")) return {false, djb2(from_utf8(str(*text, "djb2")))};
            throw std::runtime_error("bad integer " + json_text(v));
        }
        if (v.IsUint64()) return {false, v.GetUint64()};
        if (v.IsInt64()) {
            const auto s = v.GetInt64();
            return {s < 0, s < 0 ? static_cast<std::uint64_t>(-(s + 1)) + 1 : static_cast<std::uint64_t>(s)};
        }
        if (v.IsString()) {
            std::string text = str(v, "integer");
            bool negative = !text.empty() && text[0] == '-';
            if (negative) text.erase(0, 1);
            std::uint64_t value{};
            int base = 10;
            if (text.starts_with("0x") || text.starts_with("0X")) { text.erase(0, 2); base = 16; }
            const auto r = std::from_chars(text.data(), text.data() + text.size(), value, base);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) throw std::runtime_error("bad integer " + json_text(v));
            return {negative, value};
        }
        throw std::runtime_error("expected an integer, got " + json_text(v));
    }

    template <typename T> static T ranged(const Integer& i, const char* what) {
        if constexpr (std::is_signed_v<T>) {
            if (i.negative) {
                if (i.magnitude > static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + 1) throw std::runtime_error(std::string(what) + " out of range");
                return static_cast<T>(-static_cast<std::int64_t>(i.magnitude - 1) - 1);
            }
            if (i.magnitude > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) throw std::runtime_error(std::string(what) + " out of range");
            return static_cast<T>(i.magnitude);
        } else {
            if (i.negative || i.magnitude > std::numeric_limits<T>::max()) throw std::runtime_error(std::string(what) + " out of range");
            return static_cast<T>(i.magnitude);
        }
    }

    std::int64_t enum_or_int(const J& v, int enumType) {
        if (v.IsString() && !(str(v, "enum").starts_with("0x"))) {
            const auto name = str(v, "enum");
            if (enumType >= 0)
                if (const auto value = enum_value(d_, static_cast<std::size_t>(enumType), name)) return *value;
            throw std::runtime_error("unknown enum member " + name + " of " + type_name(d_, enumType));
        }
        if (v.IsObject() && member(v, "value")) return enum_or_int(*member(v, "value"), enumType);
        if (v.IsObject() && member(v, "enum")) return enum_or_int(*member(v, "enum"), enumType);
        return ranged<std::int32_t>(integer(v), "enum");
    }

    PointerReference pointer(const J& v) {
        if (v.IsNull()) return {PointerKind::null, -1};
        if (!v.IsObject()) throw std::runtime_error("a pointer is null, {\"internal\": ...} or {\"external\": ...}");
        if (const auto* internal = member(v, "internal")) return {PointerKind::internal, static_cast<std::int32_t>(instance(*internal))};
        const J* external = member(v, "external");
        if (!external && member(v, "file")) external = &v;
        if (external) {
            if (external->IsUint() || external->IsInt()) {
                const auto index = external->IsUint() ? external->GetUint() : static_cast<unsigned>(external->GetInt());
                if (index >= d_.imports.size()) throw std::runtime_error("import index out of range");
                return {PointerKind::external, static_cast<std::int32_t>(index)};
            }
            const auto file = parse_guid(str(need(*external, "file"), "file"));
            const auto inst = parse_guid(str(need(*external, "instance"), "instance"));
            const auto before = d_.imports.size();
            const auto index = ensure_import(d_, file, inst);
            if (d_.imports.size() != before) r_.lines.push_back("  import " + std::to_string(index) + " added (" + guid_text(file) + "/" + guid_text(inst) + ")");
            return {PointerKind::external, static_cast<std::int32_t>(index)};
        }
        throw std::runtime_error("bad pointer " + json_text(v));
    }

    Value scalar(FieldType type, std::uint16_t classRef, const J& v) {
        switch (type) {
        case FieldType::boolean:
            if (!v.IsBool()) throw std::runtime_error("expected true/false");
            return Value{v.GetBool()};
        case FieldType::int8: return Value{static_cast<std::int64_t>(ranged<std::int8_t>(integer(v), "int8"))};
        case FieldType::int16: return Value{static_cast<std::int64_t>(ranged<std::int16_t>(integer(v), "int16"))};
        case FieldType::int32: return Value{static_cast<std::int64_t>(ranged<std::int32_t>(integer(v), "int32"))};
        case FieldType::int64: return Value{ranged<std::int64_t>(integer(v), "int64")};
        case FieldType::uint8: return Value{static_cast<std::uint64_t>(ranged<std::uint8_t>(integer(v), "uint8"))};
        case FieldType::uint16: return Value{static_cast<std::uint64_t>(ranged<std::uint16_t>(integer(v), "uint16"))};
        case FieldType::uint32: return Value{static_cast<std::uint64_t>(ranged<std::uint32_t>(integer(v), "uint32"))};
        case FieldType::uint64: return Value{ranged<std::uint64_t>(integer(v), "uint64")};
        case FieldType::enumeration: return Value{enum_or_int(v, classRef)};
        case FieldType::float32:
        case FieldType::float64:
            if (!v.IsNumber()) throw std::runtime_error("expected a number");
            return Value{v.GetDouble()};
        case FieldType::string: {
            auto s = from_utf8(str(v, "string"));
            if (s.size() > 31) throw std::runtime_error("fixed string longer than 31 bytes");
            return Value{s};
        }
        case FieldType::cString: return Value{from_utf8(str(v, "string"))};
        case FieldType::fileRef: return Value{from_utf8(str(v.IsObject() ? need(v, "fileRef") : v, "fileRef"))};
        case FieldType::guid: return Value{parse_guid(str(v, "guid"))};
        case FieldType::resourceRef: return Value{ebx::ResourceReference{ranged<std::uint64_t>(integer(v.IsObject() ? need(v, "resource") : v), "resource")}};
        case FieldType::pointer: return Value{pointer(v)};
        case FieldType::typeRef:
        case FieldType::delegate: {
            const auto encoded = ranged<std::uint32_t>(integer(v.IsObject() ? need(v, "typeRef") : v), "typeRef");
            ebx::TypeReference t;
            t.encoded = encoded;
            if (encoded & 0x80000000U) { t.primitive = true; t.primitiveType = static_cast<FieldType>((encoded >> 5) & 0x1F); }
            else if (encoded) t.descriptor = static_cast<std::int32_t>(encoded >> 2);
            return Value{t};
        }
        default:
            throw std::runtime_error("cannot set a " + field_type_name(type) + " from JSON");
        }
    }

    Value default_value(const FieldDescriptor& field, bool element) {
        if (!element && field.category() == FieldCategory::array) return Value{Value::Array{}};
        switch (field.type()) {
        case FieldType::boolean: return Value{false};
        case FieldType::int8: case FieldType::int16: case FieldType::int32: case FieldType::int64:
        case FieldType::enumeration: return Value{std::int64_t{}};
        case FieldType::uint8: case FieldType::uint16: case FieldType::uint32: case FieldType::uint64: return Value{std::uint64_t{}};
        case FieldType::float32: case FieldType::float64: return Value{0.0};
        case FieldType::string: case FieldType::cString: case FieldType::fileRef: return Value{std::string{}};
        case FieldType::guid: return Value{fb::Guid{}};
        case FieldType::sha1: return Value{fb::Sha1{}};
        case FieldType::resourceRef: return Value{ebx::ResourceReference{}};
        case FieldType::pointer: return Value{PointerReference{PointerKind::null, -1}};
        case FieldType::typeRef: case FieldType::delegate: return Value{ebx::TypeReference{}};
        case FieldType::boxedValueRef: return Value{BoxedReference{}};
        case FieldType::structure: return Value{default_object(field.classRef)};
        default: return Value{};
        }
    }

    std::shared_ptr<Object> default_object(std::size_t typeIndex) {
        auto out = std::make_shared<Object>();
        out->descriptor = static_cast<std::int32_t>(typeIndex);
        for (const auto index : flat_fields(d_, typeIndex))
            out->fields.push_back({index, d_.fields[index].name, default_value(d_.fields[index], false)});
        return out;
    }

    void set(const Node& n, const J& v) {
        switch (n.kind) {
        case NodeKind::object: {
            if (!v.IsObject()) throw std::runtime_error(n.where + ": a struct is set from a JSON object");
            for (const auto& m : v.GetObject()) {
                Token t;
                t.name = std::string(m.name.GetString(), m.name.GetStringLength());
                set(step(n, t), m.value);
            }
            return;
        }
        case NodeKind::structured: {
            if (is_array_node(n)) {
                if (!v.IsArray()) throw std::runtime_error(n.where + ": an array is set from a JSON array (or use append)");
                Value::Array items;
                for (const auto& item : v.GetArray()) {
                    Value element = default_value(*n.field, true);
                    Node en = n;
                    en.value = &element;
                    en.element = true;
                    set(en, item);
                    items.push_back(std::move(element));
                }
                n.value->data = std::move(items);
                return;
            }
            if (n.field->type() == FieldType::structure) {
                auto* nested = std::get_if<std::shared_ptr<Object>>(&n.value->data);
                if (!nested || !*nested) n.value->data = default_object(n.field->classRef);
                nested = std::get_if<std::shared_ptr<Object>>(&n.value->data);
                Node object;
                object.kind = NodeKind::object;
                object.object = nested->get();
                object.where = n.where;
                set(object, v);
                return;
            }
            if (n.field->type() == FieldType::boxedValueRef) { set(descend_boxed(n), v); return; }
            const auto arrayHash = n.value->arrayHash;
            *n.value = scalar(n.field->type(), n.field->classRef, v);
            n.value->arrayHash = arrayHash;
            return;
        }
        case NodeKind::raw:
            set_raw(n, v);
            return;
        }
    }

    void set_raw(const Node& n, const J& v) {
        if (n.type.array) throw std::runtime_error(n.where + ": raw arrays are changed with append");
        auto& p = d_.payload;
        switch (n.type.type) {
        case FieldType::structure: {
            if (!v.IsObject()) throw std::runtime_error(n.where + ": a struct is set from a JSON object");
            for (const auto& m : v.GetObject()) {
                Token t;
                t.name = std::string(m.name.GetString(), m.name.GetStringLength());
                set(step(n, t), m.value);
            }
            return;
        }
        case FieldType::boxedValueRef: set(descend_boxed(n), v); return;
        case FieldType::pointer: {
            const auto ref = pointer(v);
            if (ref.kind == PointerKind::null) set_pointer_null(d_, n.at);
            else if (ref.kind == PointerKind::external) set_pointer_import(d_, n.at, static_cast<std::size_t>(ref.index));
            else set_pointer_instance(d_, n.at, static_cast<std::size_t>(ref.index));
            return;
        }
        case FieldType::cString: set_cstring(d_, n.at, from_utf8(str(v, "string"))); return;
        case FieldType::boolean: store<std::uint8_t>(p, n.at, v.IsBool() && v.GetBool() ? 1 : 0); return;
        case FieldType::int8: store(p, n.at, ranged<std::int8_t>(integer(v), "int8")); return;
        case FieldType::uint8: store(p, n.at, ranged<std::uint8_t>(integer(v), "uint8")); return;
        case FieldType::int16: store(p, n.at, ranged<std::int16_t>(integer(v), "int16")); return;
        case FieldType::uint16: store(p, n.at, ranged<std::uint16_t>(integer(v), "uint16")); return;
        case FieldType::int32: store(p, n.at, ranged<std::int32_t>(integer(v), "int32")); return;
        case FieldType::uint32: store(p, n.at, ranged<std::uint32_t>(integer(v), "uint32")); return;
        case FieldType::int64: store(p, n.at, ranged<std::int64_t>(integer(v), "int64")); return;
        case FieldType::uint64: store(p, n.at, ranged<std::uint64_t>(integer(v), "uint64")); return;
        case FieldType::enumeration: store(p, n.at, static_cast<std::int32_t>(enum_or_int(v, n.type.typeIndex))); return;
        case FieldType::float32: store(p, n.at, static_cast<float>(v.GetDouble())); return;
        case FieldType::float64: store(p, n.at, v.GetDouble()); return;
        case FieldType::guid: {
            const auto g = parse_guid(str(v, "guid"));
            std::memcpy(p.data() + n.at, g.bytes.data(), 16);
            return;
        }
        default:
            throw std::runtime_error(n.where + ": cannot set a raw " + field_type_name(n.type.type));
        }
    }

    // ---- ops ----
    void apply_sets(const J& sets, const std::function<Node(std::string_view)>& resolveRelative) {
        if (!sets.IsObject()) throw std::runtime_error("\"set\" must be an object of path -> value");
        for (const auto& m : sets.GetObject()) {
            const std::string path(m.name.GetString(), m.name.GetStringLength());
            set(resolveRelative(path), m.value);
            r_.lines.push_back("  set " + path + " = " + json_text(m.value).substr(0, 200));
        }
    }

    void op_clone(const J& op) {
        const auto source = instance(need(op, "src"));
        std::optional<fb::Guid> guid;
        if (const auto* g = member(op, "guid")) guid = parse_guid(str(*g, "guid"));
        std::optional<bool> exported;
        if (const auto* e = member(op, "exported")) exported = e->GetBool();
        const auto index = clone_instance(d_, st_, source, guid, exported);
        if (const auto* as = member(op, "as")) st_.names[str(*as, "as")] = index;
        r_.lines.push_back("clone_instance " + std::to_string(source) + " -> new instance " + std::to_string(index) +
                           (d_.instances[index].exported ? " guid " + guid_text(d_.instances[index].instanceGuid) : " (internal)"));
        if (const auto* sets = member(op, "set"))
            apply_sets(*sets, [&](std::string_view path) { return resolve(index, path); });
    }

    void op_set(const J& op) {
        const auto index = instance(need(op, "instance"));
        const auto path = str(need(op, "path"), "path");
        set(resolve(index, path), need(op, "value"));
        r_.lines.push_back("set instance " + std::to_string(index) + " " + path + " = " + json_text(need(op, "value")).substr(0, 200));
    }

    void op_append(const J& op) {
        const auto index = instance(need(op, "instance"));
        const auto path = str(need(op, "path"), "path");
        auto node = resolve(index, path);
        if (is_boxed(node)) node = descend_boxed(node);
        const bool deep = member(op, "deep") && member(op, "deep")->GetBool();
        const auto* cloneOf = member(op, "clone_of");
        const auto* value = member(op, "value");
        const auto* sets = member(op, "set");
        const auto* as = member(op, "as");
        std::optional<fb::Guid> deepGuid;
        if (const auto* g = member(op, "guid")) deepGuid = parse_guid(str(*g, "guid"));

        if (is_array_node(node)) {
            auto* items = std::get_if<Value::Array>(&node.value->data);
            if (!items) throw std::runtime_error(path + " holds no array value");
            Value element;
            if (cloneOf) {
                Node source = cloneOf->IsUint() ? step(node, Token{true, {}, cloneOf->GetUint()}) : resolve(index, str(*cloneOf, "clone_of"));
                if (source.kind != NodeKind::structured || !source.element || source.field != node.field)
                    throw std::runtime_error("clone_of must name an element of the same array");
                element = deep_clone_value(d_, st_, *source.value);
            } else {
                element = default_value(*node.field, true);
            }
            // structured nodes are found again after every change: appending can move the array's storage
            const auto position = items->size();
            if (deep) element = deep_pointer(element, as, deepGuid);
            items->push_back(std::move(element));
            // The array record's field-path hash (RFL2 group key) for an array that had none (it was empty).
            if (const auto* hash = member(op, "array_hash")) node.value->arrayHash = ranged<std::uint32_t>(integer(*hash), "array_hash");
            auto elementNode = [&]() {
                auto n = resolve(index, path);
                if (is_boxed(n)) n = descend_boxed(n);
                return step(n, Token{true, {}, position});
            };
            if (value) set(elementNode(), *value);
            if (sets) apply_sets(*sets, [&](std::string_view rel) { return resolve_from(elementNode(), rel); });
            r_.lines.push_back("append instance " + std::to_string(index) + " " + path + "[" + std::to_string(position) + "]" +
                               (cloneOf ? " cloned from " + json_text(*cloneOf) : ""));
            if (const auto* at = member(op, "at")) {   // the new element at that index instead of the end
                const auto to = static_cast<std::size_t>(ranged<std::uint32_t>(integer(*at), "at"));
                auto n = resolve(index, path);
                if (is_boxed(n)) n = descend_boxed(n);
                auto* list = std::get_if<Value::Array>(&n.value->data);
                if (!list || to >= list->size()) throw std::runtime_error("at: index out of range");
                std::rotate(list->begin() + static_cast<std::ptrdiff_t>(to), list->end() - 1, list->end());
                r_.lines.push_back("  moved to [" + std::to_string(to) + "]");
            }
            return;
        }
        if (node.kind == NodeKind::raw && node.type.array) {
            const auto [record, start] = raw_array(node);
            const auto stride = static_cast<std::uint32_t>(value_size(d_, node.type.type, node.type.typeIndex));
            // "after": the index right after the element whose path names that instance (found before the append)
            std::optional<std::uint32_t> afterIndex;
            if (const auto* after = member(op, "after")) {
                const auto wantPath = str(need(*after, "path"), "after.path");
                const auto want = parse_guid(str(need(*after, "instance"), "after.instance"));
                for (std::uint32_t i = 0; i < d_.arrays[record].count && !afterIndex; ++i) {
                    Node el;
                    el.kind = NodeKind::raw;
                    el.type = node.type;
                    el.type.array = false;
                    el.at = d_.arrays[record].offset + i * stride;
                    el.where = node.where + "[" + std::to_string(i) + "]";
                    try {
                        Node from_el = el;
                        if (el.type.type == FieldType::pointer) {   // an array of pointers: the instance pointed at
                            const auto v = load<std::uint64_t>(d_.payload, el.at);
                            if (v == 0 || (v & 1U)) continue;
                            const auto target = classify(d_, static_cast<std::uint32_t>(read_relative(d_, el.at).target));
                            if (target.kind != TargetKind::instance) continue;
                            from_el = root(target.index);
                        }
                        const auto n = resolve_from(from_el, wantPath);
                        if (const auto g = external_instance(n); g && *g == want) afterIndex = i + 1;
                    } catch (const std::exception&) {}
                }
                r_.lines.push_back(afterIndex ? "  after element " + std::to_string(*afterIndex - 1)
                                              : std::string("  after: no element names ") + str(need(*after, "instance"), "after.instance") +
                                                    " - left at the end");
            }
            int from = -1;
            if (cloneOf) {
                if (cloneOf->IsUint()) {
                    from = static_cast<int>(cloneOf->GetUint());
                } else {
                    const auto source = resolve(index, str(*cloneOf, "clone_of"));
                    if (source.kind != NodeKind::raw || source.at < start || (source.at - start) % stride ||
                        (source.at - start) / stride >= d_.arrays[record].count)
                        throw std::runtime_error("clone_of must name an element of the same array");
                    from = static_cast<int>((source.at - start) / stride);
                }
            }
            const auto position = d_.arrays[record].count;
            array_append(d_, st_, record, from);
            // Offsets move with every gap: the new element is found again from its record each time.
            auto elementNode = [this, record, position, stride, &node]() {
                Node out;
                out.kind = NodeKind::raw;
                out.type = node.type;
                out.type.array = false;
                out.at = d_.arrays[record].offset + position * stride;
                out.where = node.where + "[" + std::to_string(position) + "]";
                return out;
            };
            if (deep) {
                const auto en = elementNode();
                if (en.type.type != FieldType::pointer) throw std::runtime_error("deep needs an array of pointers");
                const auto v = load<std::uint64_t>(d_.payload, en.at);
                if (v == 0 || (v & 1U)) throw std::runtime_error("deep: the cloned element is not an internal pointer");
                const auto target = classify(d_, static_cast<std::uint32_t>(read_relative(d_, en.at).target));
                if (target.kind != TargetKind::instance) throw std::runtime_error("deep: the element points at no instance");
                const auto made = clone_instance(d_, st_, target.index, deepGuid, std::nullopt);
                if (as) st_.names[str(*as, "as")] = made;
                set_pointer_instance(d_, elementNode().at, made);
                r_.lines.push_back("  deep: instance " + std::to_string(target.index) + " cloned as new instance " + std::to_string(made) +
                                   (d_.instances[made].exported ? " guid " + guid_text(d_.instances[made].instanceGuid) : ""));
            }
            if (value) set(elementNode(), *value);
            if (sets) apply_sets(*sets, [&](std::string_view rel) { return resolve_from(elementNode(), rel); });
            r_.lines.push_back("append instance " + std::to_string(index) + " " + path + " (inside a boxed value) [" +
                               std::to_string(position) + "]" + (cloneOf ? " cloned from element " + std::to_string(from) : ""));
            if (const auto* at = member(op, "at")) {   // the new element at that index instead of the end
                const auto to = ranged<std::uint32_t>(integer(*at), "at");
                array_move_last(d_, st_, record, to);
                r_.lines.push_back("  moved to [" + std::to_string(to) + "]");
            } else if (afterIndex && *afterIndex < d_.arrays[record].count - 1) {
                array_move_last(d_, st_, record, *afterIndex);
                r_.lines.push_back("  moved to [" + std::to_string(*afterIndex) + "]");
            }
            return;
        }
        throw std::runtime_error(node.where + " is not an array");
    }

    // The instance an external reference at `n` names (raw or parsed), if it is one.
    std::optional<fb::Guid> external_instance(const Node& n) {
        if (n.kind == NodeKind::raw) {
            const auto v = load<std::uint64_t>(d_.payload, n.at);
            if (!(v & 1U)) return std::nullopt;
            const auto i = static_cast<std::size_t>(v >> 1);
            if (i >= d_.imports.size()) return std::nullopt;
            return d_.imports[i].classGuid;
        }
        if (n.kind == NodeKind::structured && n.value)
            if (const auto* p = std::get_if<PointerReference>(&n.value->data);
                p && p->kind == PointerKind::external && p->index >= 0 && static_cast<std::size_t>(p->index) < d_.imports.size())
                return d_.imports[static_cast<std::size_t>(p->index)].classGuid;
        return std::nullopt;
    }

    // A deep append of an internal pointer clones the instance it points at.
    Value deep_pointer(Value element, const J* as, std::optional<fb::Guid> guid) {
        auto* p = std::get_if<PointerReference>(&element.data);
        if (!p || p->kind != PointerKind::internal || p->index < 0) throw std::runtime_error("deep: the element is not an internal pointer");
        const auto made = clone_instance(d_, st_, static_cast<std::size_t>(p->index), guid, std::nullopt);
        if (as) st_.names[str(*as, "as")] = made;
        r_.lines.push_back("  deep: instance " + std::to_string(p->index) + " cloned as new instance " + std::to_string(made));
        p->index = static_cast<std::int32_t>(made);
        return element;
    }

    Document& d_;
    EditState& st_;
    ApplyResult& r_;
};

} // namespace

ApplyResult apply_ops(Document& document, EditState& state, const std::string& opsJson) {
    ApplyResult result;
    Applier(document, state, result).run(opsJson);
    return result;
}

} // namespace tool
