#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FD_RELOC_READ 1u
#define FD_RELOC_WRITE 2u
#define FD_RELOC_DUMP 4u
#define FD_RELOC_FLAGS_INIT (FD_RELOC_READ | FD_RELOC_WRITE)
struct fd_bo { uint32_t reloc_flags; struct fd_bo *backing; };
struct hash_entry { const void *key; void *data; };
struct hash_table { struct hash_entry entries[32]; unsigned count; };
struct fd_submit { unsigned unused; };
struct fd_submit_sp {
   struct fd_submit base;
   struct fd_bo *bos[32];
   unsigned nr_bos;
   struct hash_table *bo_access;
   bool bo_access_failed;
};
static bool fail_create, fail_insert;
static unsigned allocations;
static struct fd_submit_sp *to_fd_submit_sp(struct fd_submit *submit) { return (void *)submit; }
static bool suballoc_bo(struct fd_bo *bo) { return bo->backing != NULL; }
static struct fd_bo *fd_bo_heap_block(struct fd_bo *bo) { return bo->backing; }
static struct hash_table *_mesa_pointer_hash_table_create(void *parent)
{
   (void)parent;
   if (fail_create)
      return NULL;
   struct hash_table *table = calloc(1, sizeof(*table));
   assert(table);
   allocations++;
   return table;
}
static struct hash_entry *_mesa_hash_table_search(struct hash_table *table, const void *key)
{
   for (unsigned i = 0; i < table->count; i++)
      if (table->entries[i].key == key)
         return &table->entries[i];
   return NULL;
}
static struct hash_entry *_mesa_hash_table_insert(struct hash_table *table, const void *key, void *data)
{
   if (fail_insert)
      return NULL;
   struct hash_entry *entry = _mesa_hash_table_search(table, key);
   if (!entry) {
      assert(table->count < 32);
      entry = &table->entries[table->count++];
      entry->key = key;
   }
   entry->data = data;
   return entry;
}
static void _mesa_hash_table_destroy(struct hash_table *table, void *callback)
{
   (void)callback;
   if (table) {
      allocations--;
      free(table);
   }
}
static uint32_t fd_submit_append_bo(struct fd_submit_sp *submit, struct fd_bo *bo)
{
   if (suballoc_bo(bo))
      bo = fd_bo_heap_block(bo);
   for (unsigned i = 0; i < submit->nr_bos; i++)
      if (submit->bos[i] == bo)
         return i;
   assert(submit->nr_bos < 32);
   submit->bos[submit->nr_bos] = bo;
   return submit->nr_bos++;
}

#include "bo_access_under_test.h"

int main(void)
{
   struct fd_bo bo = {.reloc_flags = FD_RELOC_FLAGS_INIT | FD_RELOC_DUMP};
   struct fd_submit_sp read = {0}, plain = {0}, merged = {0};
   assert(fd_submit_sp_bo_access(&read, &bo) == 7);
   fd_submit_set_bo_access(&read.base, &bo, FD_RELOC_READ);
   assert(fd_submit_sp_bo_access(&read, &bo) == 5);
   assert(fd_submit_sp_bo_access(&plain, &bo) == 7 && bo.reloc_flags == 7);
   fd_submit_sp_append_bo_access(&merged, &bo, fd_submit_sp_bo_access(&read, &bo));
   assert(fd_submit_sp_bo_access(&merged, &bo) == 5);
   fd_submit_sp_append_bo_access(&merged, &bo, FD_RELOC_READ);
   assert(fd_submit_sp_bo_access(&merged, &bo) == 5);
   fd_submit_sp_append_bo_access(&merged, &bo, fd_submit_sp_bo_access(&plain, &bo));
   assert(fd_submit_sp_bo_access(&merged, &bo) == 7);
   fd_submit_append_bo(&plain, &bo);
   fd_submit_sp_append_bo_access(&plain, &bo, FD_RELOC_READ);
   assert(fd_submit_sp_bo_access(&plain, &bo) == 7);
   struct fd_bo a = {.backing = &bo}, b = {.backing = &bo};
   fd_submit_set_bo_access(&read.base, &a, FD_RELOC_WRITE);
   assert(fd_submit_sp_bo_access(&read, &b) == 7);
   struct fd_submit_sp failed = {0};
   fail_create = true;
   fd_submit_set_bo_access(&failed.base, &bo, FD_RELOC_READ);
   assert(failed.bo_access_failed && !failed.bo_access);
   fail_create = false;
   fd_submit_set_bo_access(&failed.base, &bo, FD_RELOC_READ);
   assert(fd_submit_sp_bo_access(&failed, &bo) == 7 && !failed.bo_access);
   struct fd_submit_sp inserted = {0};
   fd_submit_set_bo_access(&inserted.base, &bo, FD_RELOC_READ);
   fail_insert = true;
   fd_submit_set_bo_access(&inserted.base, &bo, FD_RELOC_WRITE);
   assert(inserted.bo_access_failed && !inserted.bo_access);
   fail_insert = false;
   fd_submit_set_bo_access(&inserted.base, &bo, FD_RELOC_READ);
   assert(fd_submit_sp_bo_access(&inserted, &bo) == 7 && !inserted.bo_access);
   _mesa_hash_table_destroy(read.bo_access, NULL);
   _mesa_hash_table_destroy(plain.bo_access, NULL);
   _mesa_hash_table_destroy(merged.bo_access, NULL);
   assert(allocations == 0);
   puts("BO_ACCESS PASS: read/write merging, heap aliases, per-submit isolation, OOM fallback");
   return 0;
}
