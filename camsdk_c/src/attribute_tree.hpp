#pragma once

#include <string>

namespace acam {

/* Normalize an attribute-tree JSON document to the ABI flat schema:
 *   {"attributes":[{"key":"...","type":"int|float|bool", ...optional min/max/step/default...}, ...]}
 *
 * Pass-through when the input is already flat (has an "attributes" array).
 * Vendor GenICam trees (top-level "Feature" categories) are walked and every
 * Integer / Float / Boolean / Enumeration leaf becomes one flat entry; Enum
 * maps to type "int". On parse failure the input is returned unchanged. */
std::string flatten_attribute_tree_json(const std::string& input);

} // namespace acam
