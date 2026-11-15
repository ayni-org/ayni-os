// Bounded C++17 construction of native business values; no allocation or exceptions.
#ifndef AYNI_VALUES_HPP
#define AYNI_VALUES_HPP
#include <ayni/values.h>
#include <optional>

namespace ayni {
// Owns one admitted borrowed value view, without copying user text or nodes.
// The caller keeps the source immutable through this reader's last access.
class ValueReader {
  public:
    // Failure preserves the previous view; this checks shape/representation, not Schema or Grants.
    ayni_status_t read(ayni_value_v1 value) {
        const auto status = ayni_value_validate(&value);
        if (status == AYNI_OK) {
            value_ = value;
        }
        return status;
    }
    // Null before a successful read; otherwise the original complete root node.
    const ayni_value_node_v1 *root() const { return value_ ? value_->nodes : nullptr; }
    // Direct object field only; null for absence, a scalar/list root, or before read.
    const ayni_value_node_v1 *field(uint32_t id) const {
        if (!value_ || value_->nodes[0].kind != AYNI_VALUE_OBJECT) {
            return nullptr;
        }
        for (uint32_t at = 1; at < value_->node_count; at = value_->nodes[at].subtree_end) {
            if (value_->nodes[at].field_id == id) {
                return &value_->nodes[at];
            }
        }
        return nullptr;
    }
    // Borrow a declared TEXT node; tokens/numeric values are never implicitly converted.
    static bool text(const ayni_value_node_v1 *node, ayni_string_v1 &out) {
        if (node == nullptr || node->kind != AYNI_VALUE_TEXT) {
            return false;
        }
        out = node->value.text;
        return true;
    }
    // Borrow a BODY reference node (ADR-0096); the caller reads and verifies the body.
    static bool body(const ayni_value_node_v1 *node, ayni_body_ref_v1 &out) {
        if (node == nullptr || node->kind != AYNI_VALUE_BODY) {
            return false;
        }
        out = node->value.body;
        return true;
    }

  private:
    std::optional<ayni_value_v1> value_; // None before read; otherwise a complete admitted source.
};

// Owns node/parent storage; strings borrow the caller through Core's synchronous
// copy. A sticky error prevents publication of partial trees. Since 0.48.
template <uint32_t Capacity = AYNI_VALUE_NODE_COUNT_MAX> class ValueBuilder {
  public:
    static_assert(Capacity != 0U && Capacity <= AYNI_VALUE_NODE_COUNT_MAX,
                  "native value node capacity is explicit and bounded");
    // Begin a root/list-item (field 0) or object field. end closes that subtree.
    ayni_status_t object(uint32_t field = 0) { return container(AYNI_VALUE_OBJECT, field); }
    // Preserve ordered/repeated list items; fields on items must be zero.
    ayni_status_t list(uint32_t field = 0) { return container(AYNI_VALUE_LIST, field); }
    // Close exactly the current container; its stable ID and child order are retained.
    ayni_status_t end() {
        if (status_ == AYNI_OK) {
            if (depth_ == 0U) {
                status_ = AYNI_INVALID_STATE;
            } else {
                nodes_[parents_[--depth_]].subtree_end = count_;
            }
        }
        return status_;
    }
    // All scalar constructors retain the explicit type; units/defaults belong to Schema.
    ayni_status_t null(uint32_t field = 0) {
        return scalar(AYNI_VALUE_NULL, field) ? AYNI_OK : status_;
    }
    // Publish native bool as exactly 0/1, never an integer permission coercion.
    ayni_status_t boolean(uint32_t field, bool value) {
        if (auto *node = scalar(AYNI_VALUE_BOOL, field)) {
            node->value.boolean = value ? 1U : 0U;
        }
        return status_;
    }
    // Retain the complete signed range; Schema alone supplies its unit.
    ayni_status_t integer(uint32_t field, int64_t value) {
        if (auto *node = scalar(AYNI_VALUE_I64, field)) {
            node->value.integer = value;
        }
        return status_;
    }
    // Retain the complete unsigned range without passing through a floating number.
    ayni_status_t unsigned_integer(uint32_t field, uint64_t value) {
        if (auto *node = scalar(AYNI_VALUE_U64, field)) {
            node->value.unsigned_integer = value;
        }
        return status_;
    }
    // Declare approximation explicitly; Core rejects nonfinite representations.
    ayni_status_t approximate(uint32_t field, double value) {
        if (auto *node = scalar(AYNI_VALUE_FLOAT64, field)) {
            node->value.approximate = value;
        }
        return status_;
    }
    // Retain coefficient and scale; no builder rounding or rescaling.
    ayni_status_t decimal(uint32_t field, ayni_decimal_v1 value) {
        if (auto *node = scalar(AYNI_VALUE_DECIMAL, field)) {
            node->value.decimal = value;
        }
        return status_;
    }
    // Borrow UTF-8 business text until Core copies it; no implicit NUL terminator.
    ayni_status_t text(uint32_t field, ayni_string_v1 value) {
        return string(AYNI_VALUE_TEXT, field, value);
    }
    // An EXTERNAL TEXT body reference. Side-effect arguments must already be durable;
    // explicitly asynchronous model inputs may be temporary (Since 0.55, ADR-0104).
    ayni_status_t body(uint32_t field, ayni_body_ref_v1 value) {
        if (auto *node = scalar(AYNI_VALUE_BODY, field)) {
            node->value.body = value;
        }
        return status_;
    }
    // Distinguish a catalog identifier from arbitrary business text.
    ayni_status_t identifier(uint32_t field, ayni_string_v1 value) {
        return string(AYNI_VALUE_IDENTIFIER, field, value);
    }
    // Distinguish a finite declared choice from free text.
    ayni_status_t enumeration(uint32_t field, ayni_string_v1 value) {
        return string(AYNI_VALUE_ENUM, field, value);
    }
    // Publishes only a complete tree. The view lasts until builder mutation/destruction;
    // Core still validates the representations and selected business Schema.
    ayni_status_t finish(ayni_value_v1 &out) const {
        if (status_ != AYNI_OK) {
            return status_;
        }
        if (count_ == 0U || depth_ != 0U) {
            return AYNI_INVALID_STATE;
        }
        out = {nodes_, count_};
        return AYNI_OK;
    }
    // Discards the previous tree/error; borrowed string ownership never transfers.
    void clear() {
        count_ = 0;
        depth_ = 0;
        status_ = AYNI_OK;
    }

  private:
    // This is shape construction only: no implicit kind, root or list item can
    // be inferred from an empty count; the current explicit parent decides it.
    ayni_value_node_v1 *scalar(ayni_value_kind_t kind, uint32_t field) {
        if (status_ != AYNI_OK) {
            return nullptr;
        }
        if (count_ == Capacity || depth_ == AYNI_VALUE_DEPTH_MAX) {
            status_ = AYNI_CAPACITY_EXCEEDED;
            return nullptr;
        }
        const bool object_field =
            depth_ != 0U && nodes_[parents_[depth_ - 1U]].kind == AYNI_VALUE_OBJECT;
        if ((field != 0U) != object_field || (depth_ == 0U && count_ != 0U)) {
            status_ = AYNI_INVALID_ARGUMENT;
            return nullptr;
        }
        auto &node = nodes_[count_++];
        node = {};
        node.kind = kind;
        node.field_id = field;
        node.subtree_end = count_;
        return &node;
    }
    ayni_status_t container(ayni_value_kind_t kind, uint32_t field) {
        if (scalar(kind, field)) {
            parents_[depth_++] = count_ - 1U;
        }
        return status_;
    }
    ayni_status_t string(ayni_value_kind_t kind, uint32_t field, ayni_string_v1 value) {
        if (auto *node = scalar(kind, field)) {
            node->value.text = value;
        }
        return status_;
    }
    ayni_value_node_v1 nodes_[Capacity]{};     // Occupied prefix [0,count_); text remains borrowed.
    uint32_t parents_[AYNI_VALUE_DEPTH_MAX]{}; // Open container indices, outermost first.
    uint32_t count_ = 0;             // Zero before the root; every appended node increments once.
    uint32_t depth_ = 0;             // Open containers, zero only before/after a complete tree.
    ayni_status_t status_ = AYNI_OK; // Sticky admission error, reset only by clear.
};
// Encoded bytes of value plus text more bytes of text, or 0 when it cannot be encoded. A text
// node's encoding grows by exactly its byte count, so a Provider derives a contract maximum from
// its shape with empty text instead of writing framing bytes by hand. Since 0.54.
inline uint32_t encoded_bytes(const ayni_value_v1 &value, uint32_t text = 0) {
    uint32_t size = 0;
    return ayni_value_write(&value, nullptr, 0, &size) == AYNI_BUFFER_TOO_SMALL &&
                   text <= UINT32_MAX - size
               ? size + text
               : 0U;
}
// The largest encoding of one TEXT value of at most text bytes: the root when field is zero,
// otherwise the only field of a root object. Since 0.54.
inline uint32_t text_value_bytes(uint32_t text, uint32_t field = 0) {
    ValueBuilder<2> shape;
    if (field != 0U) {
        shape.object();
    }
    shape.text(field, {});
    if (field != 0U) {
        shape.end();
    }
    ayni_value_v1 value{};
    return shape.finish(value) == AYNI_OK ? encoded_bytes(value, text) : 0U;
}
} // namespace ayni
#endif
