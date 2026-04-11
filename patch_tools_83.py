import re

with open("mybuddy.h", "r") as f:
    content = f.read()

# 1. Rename coalesce_up to coalesce_up_and_update
content = content.replace("coalesce_up(", "coalesce_up_and_update(")
content = content.replace("coalesce_up_and_update(mbd_arena_t", "coalesce_up(mbd_arena_t") # Wait, function definition
content = content.replace("static block_header_t* coalesce_up(mbd_arena_t *arena", "static block_header_t* coalesce_up_and_update(mbd_arena_t *arena")
content = content.replace("static block_header_t* coalesce_up_and_update_and_update", "static block_header_t* coalesce_up_and_update")

# 2. Remote queue back-pressure
# "If queue length > N -> force drain"
# Let's add a small length counter to remote_free_queue!
# Or probabilistic drain in alloc path.
# `mbd_alloc` fast path:
"""
    if (order <= SMALL_ORDER_MAX && data->cache[order]) {
"""
# We can just drain the remote queue if we take the slow path!
# Actually, I already drain the remote queue everywhere I take the arena lock!
# "Drain only happens when: arena lock acquired"
# BUT `mbd_alloc` fast path DOES NOT take the arena lock!
# If the thread ONLY does `mbd_alloc` fast path, the remote queue will NEVER drain!
# But wait, if it does fast path, it means it HAS memory!
# If it runs OUT of memory, it falls back to slow path!
# AND THEN it drains the remote queue!
# Is it possible that the remote queue GROWS UNBOUNDED because OTHER threads free to it?
# YES.
# So we can add an opportunistic drain in the fast path using `pthread_mutex_trylock`!
# "Or probabilistic drain in alloc path"
opportunistic_drain = """
    /* Opportunistic drain to prevent remote queue hoarding */
    if (atomic_load(&arena->remote_free_queue.head) && pthread_mutex_trylock(&arena->lock) == 0) {
        drain_remote_queue(arena);
        pthread_mutex_unlock(&arena->lock);
    }
"""
content = content.replace(
"""    if (order <= SMALL_ORDER_MAX && data->cache[order]) {""",
opportunistic_drain + "\n    if (order <= SMALL_ORDER_MAX && data->cache[order]) {")

# In mbd_free thread cache fast path:
content = content.replace(
"""    if (data->count[order] < THREAD_CACHE_SIZE) {""",
opportunistic_drain + "\n    if (data->count[order] < THREAD_CACHE_SIZE) {")

with open("mybuddy.h", "w") as f:
    f.write(content)
