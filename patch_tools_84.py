with open("mybuddy.h", "r") as f:
    content = f.read()

# Let's fix the rename properly
content = content.replace("coalesce_up_and_update(mbd_arena_t *arena, block_header_t **block, uint32_t *order_out)", "coalesce_up_and_update(mbd_arena_t *arena, block_header_t *block, uint32_t *order_out)")
content = content.replace("coalesce_up(", "coalesce_up_and_update(")
content = content.replace("coalesce_up_and_update_and_update(", "coalesce_up_and_update(")

with open("mybuddy.h", "w") as f:
    f.write(content)

with open("SCALING_PLAN.md", "r") as f:
    content = f.read()

content = content.replace("- [ ] Enforce a hard cap on per-thread caches using global pressure awareness", "- [x] Enforce a hard cap on per-thread caches using global pressure awareness")
content = content.replace("- [ ] Add thread cache decay/aging", "- [x] Add thread cache decay/aging")

with open("SCALING_PLAN.md", "w") as f:
    f.write(content)
