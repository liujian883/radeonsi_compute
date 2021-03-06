#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include "computesi.h"

#define PKT3C(a, b, c) (PKT3(a, b, c) | 1 << 1)

#define set_compute_reg(reg, val) do {\
  assert(reg >= SI_SH_REG_OFFSET && reg <= SI_SH_REG_END); \
  buf[cdw++] = PKT3C(PKT3_SET_SH_REG, 1, 0); \
  buf[cdw++] = (reg - SI_SH_REG_OFFSET) >> 2; \
  buf[cdw++] = val; \
  }while (0)

#ifndef RADEON_VA_MAP
   
#define RADEON_VA_MAP               1
#define RADEON_VA_UNMAP             2
       
#define RADEON_VA_RESULT_OK         0
#define RADEON_VA_RESULT_ERROR      1
#define RADEON_VA_RESULT_VA_EXIST   2
       
#define RADEON_VM_PAGE_VALID        (1 << 0)
#define RADEON_VM_PAGE_READABLE     (1 << 1)
#define RADEON_VM_PAGE_WRITEABLE    (1 << 2)
#define RADEON_VM_PAGE_SYSTEM       (1 << 3)
#define RADEON_VM_PAGE_SNOOPED      (1 << 4)

#define RADEON_CHUNK_ID_FLAGS       0x03
#define RADEON_CS_USE_VM            0x02
#define RADEON_CS_RING_GFX          0
#define RADEON_CS_RING_COMPUTE      1

struct drm_radeon_gem_va {
    uint32_t    handle;
    uint32_t    operation;
    uint32_t    vm_id;
    uint32_t    flags;
    uint64_t    offset;
};

#define DRM_RADEON_GEM_VA   0x2b
#endif

#ifndef RADEON_INFO_VA_START
  #define RADEON_INFO_VA_START          0x0e
  #define RADEON_INFO_IB_VM_MAX_SIZE    0x0f
#endif

struct cs_reloc_gem {
    uint32_t    handle;
    uint32_t    read_domain;
    uint32_t    write_domain;
    uint32_t    flags;
};

struct compute_context* compute_create_context(const char* drm_devfile)
{
  struct drm_radeon_info ginfo;
  assert(drmAvailable());
  struct compute_context* ctx = malloc(sizeof(struct compute_context));
  
  ctx->fd = open(drm_devfile, O_RDWR, 0);
  
  if (ctx->fd < 1)
  {
    free(ctx);
    return NULL;
  }

  uint64_t reserved_mem = 0;
  uint64_t max_vm_size = 0;
  
  memset(&ginfo, 0, sizeof(ginfo));
  ginfo.request = RADEON_INFO_VA_START;
  ginfo.value = (uintptr_t)&reserved_mem;
  
  if (drmCommandWriteRead(ctx->fd, DRM_RADEON_INFO, &ginfo, sizeof(ginfo)))
  {
    close(ctx->fd);
    free(ctx);
    return NULL;
  }
  
  ginfo.request = RADEON_INFO_IB_VM_MAX_SIZE;
  ginfo.value = (uintptr_t)&max_vm_size;
  
  if (drmCommandWriteRead(ctx->fd, DRM_RADEON_INFO, &ginfo, sizeof(ginfo)))
  {
    close(ctx->fd);
    free(ctx);
    return NULL;
  }
  
  printf("reserved mem: 0x%lx vm size: 0x%lx pages\n", reserved_mem, max_vm_size);
  
  ctx->vm_pool = malloc(sizeof(struct pool_node));
  ctx->vm_pool->va = 0;
  ctx->vm_pool->size = reserved_mem+4096; ///reserved VM area by the driver 
  ctx->vm_pool->prev = NULL;
  ctx->vm_pool->next = NULL;

  return ctx;
}

void compute_free_context(struct compute_context* ctx)
{
  while (ctx->vm_pool->next)
  {
    compute_free_gpu_buffer(ctx->vm_pool->next->bo);
  }
  
  free(ctx->vm_pool);
  close(ctx->fd);
  free(ctx);
}

uint64_t compute_pool_alloc(struct compute_context* ctx, uint64_t size, int alignment, struct gpu_buffer* bo)
{
  struct pool_node *n;
  assert((size & 4095) == 0);

  for (n = ctx->vm_pool; n; n = n->next)
  {
    if (n->next)
    {
      if ((int64_t)n->next->va - n->va - n->size > size && alignment <= 4096)
      {
        struct pool_node* n2 = malloc(sizeof(struct pool_node));
        
        n2->bo = bo;
        n2->va = n->va + n->size;
        n2->size = size;
        n2->prev = n;
        n2->next = n->next;
        n->next->prev = n2;
        n->next = n2;
        
        return n2->va;
      }
    }
    else
    {
      struct pool_node* n2 = malloc(sizeof(struct pool_node));
      
      n2->bo = bo;
      n2->va = n->va + n->size + 4096;
      n2->size = size;
      n->next = n2;
      n2->prev = n;
      n2->next = NULL;
      
      return n2->va;
    }
  }
  
  assert(0 && "unreachable");
  return 0;
}

void compute_pool_free(struct compute_context* ctx, uint64_t va)
{
  struct pool_node *n;
  
  assert(va > 0);
  
  for (n = ctx->vm_pool; n; n = n->next)
  {
    if (n->va == va)
    {
      n->prev->next = n->next;
      
      if (n->next)
      {
        n->next->prev = n->prev;
      }
      
      free(n);
      return;
    }
  }
  
  assert(0 && "internal error attempted to free a non allocated vm block");
}

static int compute_vm_map(struct compute_context* ctx, uint64_t vm_addr, uint32_t handle, int vm_id, int flags)
{
  struct drm_radeon_gem_va va;
  int r;
  
  memset(&va, 0, sizeof(va));
  
  va.handle = handle;
  va.vm_id = vm_id;
  va.operation = RADEON_VA_MAP;
  va.flags = flags |
             RADEON_VM_PAGE_READABLE |
             RADEON_VM_PAGE_WRITEABLE;
             
  va.offset = vm_addr;
  
  r = drmCommandWriteRead(ctx->fd, DRM_RADEON_GEM_VA, &va, sizeof(va));
  
  if (r && va.operation == RADEON_VA_RESULT_ERROR)
  {
    fprintf(stderr, "radeon: Failed to map buffer: %x\n", handle);
    return -1;
  }
  
  if (va.operation == RADEON_VA_RESULT_VA_EXIST)
  {
    fprintf(stderr, "double map?\n");
//     assert(0 && "This cannot happen!");
    return -1;
  }
  
  return 0;
}

int compute_vm_remap(struct gpu_buffer* bo)
{
  return compute_vm_map(bo->ctx, bo->va, bo->handle, 0, RADEON_VM_PAGE_SNOOPED);
}

static int compute_vm_unmap(struct compute_context* ctx, uint64_t vm_addr, uint32_t handle, int vm_id)
{
  return 0; //WARNING
  struct drm_radeon_gem_va va;
  int r;
  
  memset(&va, 0, sizeof(va));
  
  va.handle = handle;
  va.vm_id = vm_id;
  va.operation = RADEON_VA_UNMAP;
  va.flags = RADEON_VM_PAGE_SNOOPED; ///BUG: kernel should ignore this flag for unmap,but still asserts it
             
  va.offset = vm_addr;
  
  r = drmCommandWriteRead(ctx->fd, DRM_RADEON_GEM_VA, &va, sizeof(va));
  
  if (r && va.operation == RADEON_VA_RESULT_ERROR)
  {
    fprintf(stderr, "radeon: Failed to unmap buffer: %x\n", handle);
    return -1;
  }
  
  return 0;
}

static struct cs_reloc_gem* compute_create_reloc_table(const struct compute_context* ctx, int* size)
{
  struct cs_reloc_gem* relocs = NULL;
  struct pool_node *n;
  int i = 0;
  
  for (n = ctx->vm_pool->next; n; n = n->next)
  {
    (*size)++;
  }
  
  if (*size == 0)
  {
    return NULL;
  }
  
  relocs = calloc(*size, sizeof(struct cs_reloc_gem));
  
  i = 0;
  
  for (n = ctx->vm_pool->next; n; n = n->next)
  {
    relocs[i].handle = n->bo->handle;
    relocs[i].read_domain = n->bo->domain;
    relocs[i].write_domain = n->bo->domain;
    relocs[i].flags = 0;
    i++;
  }
  
  return relocs;
}

void compute_free_gpu_buffer(struct gpu_buffer* bo)
{
  struct drm_gem_close args;
  
  if (bo->va)
  {
    compute_pool_free(bo->ctx, bo->va);
    compute_vm_unmap(bo->ctx, bo->va, bo->handle, 0);
  }
  
  memset(&args, 0, sizeof(args));
  args.handle = bo->handle;
  drmIoctl(bo->ctx->fd, DRM_IOCTL_GEM_CLOSE, &args); 
  free(bo);
}


struct gpu_buffer* compute_alloc_gpu_buffer(struct compute_context* ctx, int size, int domain, int alignment)
{
  struct drm_radeon_gem_create args;
  struct gpu_buffer* buf = calloc(1, sizeof(struct gpu_buffer));
  
  memset(&args, 0, sizeof(args));
  args.size = size;
  args.alignment = alignment;
  args.initial_domain = domain;
  
  if (drmCommandWriteRead(ctx->fd, DRM_RADEON_GEM_CREATE, &args, sizeof(args)))
  {
    fprintf(stderr, "radeon: Failed to allocate a buffer:\n");
    fprintf(stderr, "radeon:    size      : %d bytes\n", size);
    fprintf(stderr, "radeon:    alignment : %d bytes\n", alignment);
    fprintf(stderr, "radeon:    domains   : %d\n", domain);
    return NULL;
  }
  
  fprintf(stderr, "handle: %i\n", args.handle);
  
  buf->ctx = ctx;
  buf->alignment = args.alignment;
  buf->handle = args.handle;
  buf->domain = args.initial_domain;
  buf->flags = 0;
  buf->size = size;
  
  buf->va_size = ((int)((size + 4095) / 4096)) * 4096;
        
  buf->va = compute_pool_alloc(ctx, buf->va_size, buf->alignment, buf);
                                      
  if (compute_vm_map(ctx, buf->va, buf->handle, 0, RADEON_VM_PAGE_SNOOPED))
  {
    compute_pool_free(ctx, buf->va);
    buf->va = 0;
    compute_free_gpu_buffer(buf);
    return NULL;
  }
  
  return buf;
}

int compute_bo_wait(struct gpu_buffer *boi)
{
    struct drm_radeon_gem_wait_idle args;
    int ret;
    
    /* Zero out args to make valgrind happy */
    memset(&args, 0, sizeof(args));
    args.handle = boi->handle;
    do {
        ret = drmCommandWriteRead(boi->ctx->fd, DRM_RADEON_GEM_WAIT_IDLE,
                                  &args, sizeof(args));
    } while (ret == -EBUSY);
    return ret;
}

void compute_flush_caches(const struct compute_context* ctx)
{
  struct drm_radeon_cs cs;
  unsigned buf[1024];
  int cdw = 0;
  uint64_t chunk_array[5];
  struct drm_radeon_cs_chunk chunks[5];
  uint32_t flags[3];
  struct cs_reloc_gem* relocs;

  buf[cdw++] = PKT3C(PKT3_SURFACE_SYNC, 3, 0);
  buf[cdw++] = S_0085F0_TCL1_ACTION_ENA(1) |
               S_0085F0_SH_ICACHE_ACTION_ENA(1) |
               S_0085F0_SH_KCACHE_ACTION_ENA(1) |
               S_0085F0_TC_ACTION_ENA(1);

  buf[cdw++] = 0xffffffff;
  buf[cdw++] = 0;
  buf[cdw++] = 0xA;

  flags[0] = RADEON_CS_USE_VM;
  flags[1] = RADEON_CS_RING_COMPUTE;
  
  chunks[0].chunk_id = RADEON_CHUNK_ID_FLAGS;
  chunks[0].length_dw = 2;
  chunks[0].chunk_data =  (uint64_t)(uintptr_t)&flags[0];

  #define RELOC_SIZE (sizeof(struct cs_reloc_gem) / sizeof(uint32_t))
  
  int reloc_num = 0;
  relocs = compute_create_reloc_table(ctx, &reloc_num);
  
  chunks[1].chunk_id = RADEON_CHUNK_ID_RELOCS;
  chunks[1].length_dw = reloc_num*RELOC_SIZE;
  chunks[1].chunk_data =  (uint64_t)(uintptr_t)relocs;

  chunks[2].chunk_id = RADEON_CHUNK_ID_IB;
  chunks[2].length_dw = cdw;
  chunks[2].chunk_data =  (uint64_t)(uintptr_t)&buf[0];  

  printf("cdw: %i\n", cdw);

  chunk_array[0] = (uint64_t)(uintptr_t)&chunks[0];
  chunk_array[1] = (uint64_t)(uintptr_t)&chunks[1];
  chunk_array[2] = (uint64_t)(uintptr_t)&chunks[2];
  
  cs.num_chunks = 3;
  cs.chunks = (uint64_t)(uintptr_t)chunk_array;
  cs.cs_id = 1;
  
  int r = drmCommandWriteRead(ctx->fd, DRM_RADEON_CS, &cs, sizeof(struct drm_radeon_cs));

  printf("ret:%i\n", r);

  free(relocs);
}

int compute_emit_compute_state(const struct compute_context* ctx, const struct compute_state* state)
{
  struct drm_radeon_cs cs;
  int i, r;
  unsigned buf[1024];
  int cdw = 0;
  uint64_t chunk_array[5];
  struct drm_radeon_cs_chunk chunks[5];
  uint32_t flags[3];
  struct cs_reloc_gem* relocs;
  
  set_compute_reg(R_00B804_COMPUTE_DIM_X,         state->dim[0]);
  set_compute_reg(R_00B808_COMPUTE_DIM_Y,         state->dim[1]);
  set_compute_reg(R_00B80C_COMPUTE_DIM_Z,         state->dim[2]);
  set_compute_reg(R_00B810_COMPUTE_START_X,       state->start[0]);
  set_compute_reg(R_00B814_COMPUTE_START_Y,       state->start[1]);
  set_compute_reg(R_00B818_COMPUTE_START_Z,       state->start[2]);
  
  set_compute_reg(R_00B81C_COMPUTE_NUM_THREAD_X,  S_00B81C_NUM_THREAD_FULL(state->num_thread[0]));
  set_compute_reg(R_00B820_COMPUTE_NUM_THREAD_Y,  S_00B820_NUM_THREAD_FULL(state->num_thread[1]));
  set_compute_reg(R_00B824_COMPUTE_NUM_THREAD_Z,  S_00B824_NUM_THREAD_FULL(state->num_thread[2]));
  
  set_compute_reg(R_00B82C_COMPUTE_MAX_WAVE_ID,   S_00B82C_MAX_WAVE_ID(0x200));
  
  set_compute_reg(R_00B830_COMPUTE_PGM_LO,        state->binary->va >> 8);
  set_compute_reg(R_00B834_COMPUTE_PGM_HI,        state->binary->va >> 40);
  
  set_compute_reg(R_00B848_COMPUTE_PGM_RSRC1,
    S_00B848_VGPRS(state->vgpr_num) |  S_00B848_SGPRS(state->sgpr_num) |  S_00B848_PRIORITY(state->priority) |
    S_00B848_FLOAT_MODE(0) | S_00B848_PRIV(0) | S_00B848_DX10_CLAMP(0) |
    S_00B848_DEBUG_MODE(state->debug_mode) | S_00B848_IEEE_MODE(state->ieee_mode)
  );
  
  set_compute_reg(R_00B84C_COMPUTE_PGM_RSRC2,
    S_00B84C_SCRATCH_EN(state->scratch_en) | S_00B84C_USER_SGPR(state->user_data_length) |
    S_00B84C_TGID_X_EN(1) | S_00B84C_TGID_Y_EN(1) | S_00B84C_TGID_Z_EN(1) |
    S_00B84C_TG_SIZE_EN(1) |
    S_00B84C_TIDIG_COMP_CNT(0) |
    S_00B84C_LDS_SIZE(state->lds_size) |
    S_00B84C_EXCP_EN(state->excp_en)
  );
  
  set_compute_reg(R_00B854_COMPUTE_RESOURCE_LIMITS,
    S_00B854_WAVES_PER_SH(state->waves_per_sh) | S_00B854_TG_PER_CU(state->thread_groups_per_cu) |
    S_00B854_LOCK_THRESHOLD(state->lock_threshold) | S_00B854_SIMD_DEST_CNTL(state->simd_dest_cntl) 
  );
  
  set_compute_reg(R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0,
    S_00B858_SH0_CU_EN(state->se0_sh0_cu_en) | S_00B858_SH1_CU_EN(state->se0_sh1_cu_en)
  );
  
  set_compute_reg(R_00B85C_COMPUTE_STATIC_THREAD_MGMT_SE1,
    S_00B85C_SH0_CU_EN(state->se1_sh0_cu_en) | S_00B85C_SH1_CU_EN(state->se1_sh1_cu_en)
  );
  
  
  if (state->user_data_length)
  {
    buf[cdw++] = PKT3C(PKT3_SET_SH_REG, state->user_data_length, 0);
    buf[cdw++] = (R_00B900_COMPUTE_USER_DATA_0 - SI_SH_REG_OFFSET) >> 2;

    for (i = 0; i < state->user_data_length; i++)
    {
      buf[cdw++] = state->user_data[i];
    }
  }


  buf[cdw++] = PKT3C(PKT3_SURFACE_SYNC, 3, 0);
  buf[cdw++] = S_0085F0_TCL1_ACTION_ENA(1) |
               S_0085F0_SH_ICACHE_ACTION_ENA(1) |
               S_0085F0_SH_KCACHE_ACTION_ENA(1) |
               S_0085F0_TC_ACTION_ENA(1);

  buf[cdw++] = 0xffffffff;
  buf[cdw++] = 0;
  buf[cdw++] = 0xA;

  set_compute_reg(R_00B800_COMPUTE_DISPATCH_INITIATOR,
    S_00B800_COMPUTE_SHADER_EN(1) | S_00B800_PARTIAL_TG_EN(0) |
    S_00B800_FORCE_START_AT_000(0) | S_00B800_ORDERED_APPEND_ENBL(0) 
  );
  
  set_compute_reg(R_00B800_COMPUTE_DISPATCH_INITIATOR, 0);

  flags[0] = RADEON_CS_USE_VM;
  flags[1] = RADEON_CS_RING_COMPUTE;
  
  chunks[0].chunk_id = RADEON_CHUNK_ID_FLAGS;
  chunks[0].length_dw = 2;
  chunks[0].chunk_data =  (uint64_t)(uintptr_t)&flags[0];

  #define RELOC_SIZE (sizeof(struct cs_reloc_gem) / sizeof(uint32_t))
  
  int reloc_num = 0;
  relocs = compute_create_reloc_table(ctx, &reloc_num);
  
  chunks[1].chunk_id = RADEON_CHUNK_ID_RELOCS;
  chunks[1].length_dw = reloc_num*RELOC_SIZE;
  chunks[1].chunk_data =  (uint64_t)(uintptr_t)relocs;

  chunks[2].chunk_id = RADEON_CHUNK_ID_IB;
  chunks[2].length_dw = cdw;
  chunks[2].chunk_data =  (uint64_t)(uintptr_t)&buf[0];  

//   printf("cdw: %i\n", cdw);

  chunk_array[0] = (uint64_t)(uintptr_t)&chunks[0];
  chunk_array[1] = (uint64_t)(uintptr_t)&chunks[1];
  chunk_array[2] = (uint64_t)(uintptr_t)&chunks[2];
  
  cs.num_chunks = 3;
  cs.chunks = (uint64_t)(uintptr_t)chunk_array;
  cs.cs_id = state->id;
  
  r = drmCommandWriteRead(ctx->fd, DRM_RADEON_CS, &cs, sizeof(struct drm_radeon_cs));

//   printf("ret:%i\n", r);
  
  compute_bo_wait(state->binary); ///to see if it hangs
  
  free(relocs);
  
  return r;
}

int compute_copy_to_gpu(struct gpu_buffer* bo, int gpu_offset, const void* src, int size)
{
  struct drm_radeon_gem_mmap args;
  int r;
  void* ptr;
  
  if (size > bo->size + gpu_offset)
  {
    return -1;
  }
  
  memset(&args, 0, sizeof(args));
  
  args.handle = bo->handle;
  args.offset = gpu_offset;
  args.size = (uint64_t)size;
  r = drmCommandWriteRead(bo->ctx->fd,
                          DRM_RADEON_GEM_MMAP,
                          &args,
                          sizeof(args));
  if (r)
  {
    fprintf(stderr, "error mapping %p 0x%08X (error = %d)\n", bo, bo->handle, r);
    return -2;
  }
  
 ptr = mmap(0, args.size, PROT_READ|PROT_WRITE, MAP_SHARED, bo->ctx->fd, args.addr_ptr);
 
 if (ptr == MAP_FAILED)
 {
   fprintf(stderr, "mmap failed: %s\n", strerror(errno));
    return -3;
 }
 
 memcpy(ptr, src, size);
 munmap(ptr, args.size);
 
 return 0;
}

int compute_copy_from_gpu(struct gpu_buffer* bo, int gpu_offset, void* dst, int size)
{
  struct drm_radeon_gem_mmap args;
  int r;
  void* ptr;
  
  if (size > bo->size + gpu_offset)
  {
    return -1;
  }
  
  memset(&args, 0, sizeof(args));
  
  args.handle = bo->handle;
  args.offset = gpu_offset;
  args.size = (uint64_t)size;
  r = drmCommandWriteRead(bo->ctx->fd,
                          DRM_RADEON_GEM_MMAP,
                          &args,
                          sizeof(args));
  if (r)
  {
    fprintf(stderr, "error mapping %p 0x%08X (error = %d)\n", bo, bo->handle, r);
    return -2;
  }
  
 ptr = mmap(0, args.size, PROT_READ|PROT_WRITE, MAP_SHARED, bo->ctx->fd, args.addr_ptr);
 
 if (ptr == MAP_FAILED)
 {
   fprintf(stderr, "mmap failed: %s\n", strerror(errno));
    return -3;
 }
 
 memcpy(dst, ptr, size);
 munmap(ptr, args.size);
 
 return 0;
}

