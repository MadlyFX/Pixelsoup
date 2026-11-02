#include "catalog_parts.hpp"

namespace ajfx::studio {

using namespace catalog_controls;

// Expert: one raw module.
void add_expert_blocks(std::vector<BlockDef>& blocks) {
    blocks.push_back(BlockDef{
        .kind = BlockKind::Module,
        .title = "Module",
        .description = "One raw ajfx module, every field editable.",
        .category = Category::Expert,
        .controls = {},
        .keywords = "raw expert advanced module custom",
        .detachable = false,
    });
}

} // namespace ajfx::studio
