/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
//#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unordered_map>

#include "slabs.h"
#include "settings.h"
#include "log.h"
#include "kernel.h"

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */
/*slabs_clsid函数是SlabAllocator类中的一个辅助函数，用于根据给定的内存大小(size)确定合适的Slab类ID。
每个Slab类对应不同大小的内存块(chunk),该函数的主要功能是：
1.根据内存大小选择合适的Slab类。
2.确保分配的内存块能够容纳指定大小的对象。
3.返回对应的Slab类ID，用于后续的内存分配操作。
参数：size——需要分配的内存大小(以字节为单位)
返回值：
返回合适的Slab类ID(unsigned int类型)
如果无法找到合适的Slab类，返回0，表示错误。*/
unsigned int SlabAllocator::slabs_clsid(const size_t size) {
  int res = POWER_SMALLEST;//用于存储当前的Slab类ID，初始值为POWER_SMALLEST，是一个常量，表示最小的Slab类ID 

  if (size == 0)//如果size为0，直接返回0，表示错误。因为分配大小为0的内存没有意义，函数直接返回错误
    return 0;
  /*遍历所有的Slab类，检查当前Slab类的块大小slabclass[res].size是否能够容纳size。如果size大于当前Slab类的块大小，则递增res，继续检查下一个Slab类。
  边界条件：如果res达到power_larger(表示最大的Slab类ID)，仍然没有找到合适的块大小，则返回0，表示无法找到合适的Slab类。*/
  while (size > slabclass[res].size) {
    if (res++ == power_largest) /* won't fit in the biggest slab */
      return 0;
  }
  return res; //如果找到合适的Slab类，则返回对应的Slab类ID(res) 
}
/*mmap_malloc函数是SlabAllcator类中的一个内存分配函数，使用mmap系统调用分配内存。它的主要功能是：
1.分配一块对其的内存
2.支持大页内存(Huge Pages)分配(可选)
3.确保分配的内存地址对齐到指定的块大小(BLOCK_SIZE).*/
void* SlabAllocator::mmap_malloc(size_t size) { //size:需要分配的内存大小（以字节为单位）
  static void *fixed_base = NULL;  //(void *) (0x7fc435400000); 静态变量，表示固定的内存基地址(默认为NULL)。如果需要分配固定地址的内存，可以设置fixed_base。当前代码中设置为NULL，则未使用固定地址。
  epicLog(LOG_INFO, "mmap_malloc size  = %ld", size); //打印分配请求的大小
  void* ret;
  size_t aligned_size = size + BLOCK_SIZE; //预留额外的对齐空间，以防地址对齐后剩余空间不足所要求的预分配空间大小
  if (aligned_size % BLOCK_SIZE) {//如果size不是块大小BLOCK_SIZE的整数倍，则将其对齐到最近的BLOCK_SIZE倍数
    size_t old_size = aligned_size;
    aligned_size = ALIGN(aligned_size, BLOCK_SIZE);//将size对齐到BLOCK_SIZE的倍数。假设BLOCK_SIZE为4096(4K)，则ALIGN(size, BLOCK_SIZE)会将size向上对齐到最接近的4K的倍数。5000-8192;4096-4096
    epicLog(LOG_WARNING, "aligned the size from %lu to %lu", old_size, aligned_size);
  }
#ifdef USE_HUGEPAGE  //调用mmap系统调用分配内存  
  ret = mmap(fixed_base, aligned_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_HUGETLB, -1, 0);
#else
/* 参数说明：
 * 1. fixed_base：指定分配内存的起始地址，如果为NULL，则由内核选择地址。
 * 2. size：要分配的内存大小。
 * 3. PROT_READ | PROT_WRITE：内存的访问权限，表示可读可写。
 * 4. MAP_PRIVATE | MAP_ANON：映射类型，MAP_PRIVATE表示私有映射，MAP_ANON表示匿名映射。
 * 5. MAP_HUGETLB：表示使用大页内存映射。
 * -1，0:表示不与文件关联
 * 返回值：成功时返回映射的内存地址，失败时返回MAP_FAILED。
 * 该函数用于分配一块内存，返回值为分配的内存地址。返回的内存地址是对齐到指定块大小的。 
 */
  ret = mmap(fixed_base, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
  if (ret == MAP_FAILED) {  //#define MAP_FAILED      ((void *)-1)
    perror("map failed");
    return NULL;//如果mmap返回MAP_FAILED，表示分配失败，打印错误信息并返回NULL
  }
  uint64_t uret = (uint64_t) ret;
  /*如果返回的内存地址不是块大小BLOCK_SIZE的整数倍，则将其向上对齐到最近的BLOCK_SIZE倍数。对齐的原因
  1.确保分配的内存地址满足对齐要求，提高内存访问效率。
  2.某些硬件或应用程序可能要求内存地址对齐到特定的边界。*/
  if (uret % BLOCK_SIZE) {
    uret += (BLOCK_SIZE - (uret % BLOCK_SIZE));
  }
  ret = (void*) uret;

//	if(posix_memalign(&ret, BLOCK_SIZE, size)){
//		epicLog(LOG_FATAL, "allocate memory %ld failed (%d:%s)", size, errno, strerror(errno));
//	}
  return ret;  //返回分配的内存地址。如果分配失败，返回NULL
}

void SlabAllocator::mmap_free(void* ptr) {
  uint64_t uptr = (uint64_t) ptr;
  if (uptr % BLOCK_SIZE) {
    uptr -= (BLOCK_SIZE - (uptr % BLOCK_SIZE));
  }
  munmap((void*) uptr, mem_limit);
  //free(ptr);
}

size_t SlabAllocator::get_avail() {
  return mem_free;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
/*该函数用于初始化SlabAllocator类的内存分配器。它确定内存块(chunk)的大小，并相应地初始化slab类描述符。
该函数还可以选择预分配所有内存或按需分配内存。
const size_t limit：内存限制，表示最大可分配的内存大小。
const double factor：增长因子，每个slab的块大小是前一个slab的块大小乘以这个因子。
const bool prealloc：是否预分配所有内存。如果为1，则预分配所有内存；如果为0，则按需分配内存。
设计和意义：
1.高效内存管理：通过预分配内存块(slabs)来管理内存，减少内存碎片，提高内存分配和释放度的效率。支持预分配和按需分配两种策略，用户可按需选择。
2.灵活的内存分配策略：通过增长因子确定每个slab的块大小，支持灵活的内存分配策略。确保内存块的对齐，提高内存访问效率。
3.内存统计和监控：提供内存分配的统计信息，方便监控和调试。支持测试套件的初始分配，方便测试和验证。*/
void* SlabAllocator::slabs_init(const size_t limit, const double factor,
                                const bool prealloc) {
  epicLog(LOG_DEBUG, "limit = %ld, factor = %lf, prealloc = %d\n", limit,
          factor, prealloc); //初始化日志
  //初始化变量
  int i = POWER_SMALLEST - 1; //用于遍历slab类数组 
  unsigned int size = SB_PREFIX_SIZE + chunk_size; //初始化块大小，包括前缀大小和默认块大小
  unsigned int pre_size = size; //记录前一个块大小 
  mem_limit = limit; //设置最大内存限制
  mem_free = mem_limit; //初始化为mem_limit，表示当前可用内存 
  //预分配内促
  if (prealloc) {
    /* Allocate everything in a big chunk with malloc */
    //hack by zh
    mem_base = (char*) mmap_malloc(mem_limit); //如果prealloc为true，调用mmap_malloc分配一大块内存，mem_base指向分配的内存基地址
    if (mem_base != NULL) {
      dbprintf("allocate succeed\n");
      mem_current = mem_base; //指向当前可用内存的起始地址
      mem_avail = mem_limit; //记录当前可用内存大小
    } else { //如果分配失败，打印警告信息
      fprintf(stderr, "Warning: Failed to allocate requested memory in"
              " one large chunk.\nWill allocate in smaller chunks\n");
    }
  }
  //初始化slab类数组
  memset(slabclass, 0, sizeof(slabclass)); //清空slabclass数组，将其清零，初始化所有slab类描述符 
  //确定块大小并初始化slab类描述符
  while (++i < POWER_LARGEST && size <= item_size_max / factor) {
    /* Make sure items are always n-byte aligned */
    if (size % CHUNK_ALIGN_BYTES) //确保块大小是CHUNK_ALIGN_BYTES的倍数
      size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES); //如果不是，调整块大小以对齐

    if ((int) (pre_size / BLOCK_SIZE) < (int) (size / BLOCK_SIZE) //初始化slab类描述符
        && (size % BLOCK_SIZE)) {
      slabclass[i].size = size / BLOCK_SIZE * BLOCK_SIZE; //当前slab类的块大小
      slabclass[i].perslab = item_size_max / slabclass[i].size; //每个slab类中块的数量，计算公式如程序所示
      //epicLog(LOG_DEBUG, "aligned slab to slabclass[i].size = %d", slabclass[i].size);
      i++;
    }

    slabclass[i].size = size;//当前slab类的块大小
    slabclass[i].perslab = item_size_max / slabclass[i].size;//每个slab类中块的数量，计算公式如程序所示
    pre_size = size;
    size *= factor; //更新块大小，块大小乘以增长因子，得到下一个slab类的块大小
//  epicLog(LOG_DEBUG, "slab class %3d: chunk size %9u perslab %7u\n",
//          i, slabclass[i].size, slabclass[i].perslab);
  }
  //设置最大slab类
  power_largest = i;
  slabclass[power_largest].size = item_size_max; //设置最大slab类的块大小为item_size_max，每个slab中只有一个块
  slabclass[power_largest].perslab = 1;
  epicLog(LOG_DEBUG, "slab class %3d: chunk size %9u perslab %7u\n", i,
          slabclass[i].size, slabclass[i].perslab);

  //测试套件的初始分配
  /* for the test suite:  faking of how much we've already malloc'd */
  {
    char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC"); //减产环境变量T_MEMD_INITIAL_MALLOC
    if (t_initial_malloc) {//如果存在
      mem_malloced = (size_t) atol(t_initial_malloc); //设置mem_malloced为初始分配的内存大小
    }

  }
  //预分配slab
  if (prealloc) {
    slabs_preallocate(power_largest); //调用slabs_preallocate为每个slab类与分配内存
  }
  //返回内存基地址
  return mem_base; //返回分配的内存基地址
}

void SlabAllocator::slabs_preallocate(const unsigned int maxslabs) {
  int i;
  unsigned int prealloc = 0;

  /* pre-allocate a 1MB slab in every size class so people don't get
   confused by non-intuitive "SERVER_ERROR out of memory"
   messages.  this is the most common question on the mailing
   list.  if you really don't want this, you can rebuild without
   these three lines.  */

  for (i = POWER_SMALLEST; i <= POWER_LARGEST; i++) {
    if (++prealloc > maxslabs)
      return;
    if (do_slabs_newslab(i) == 0) {
      fprintf(stderr, "Error while preallocating slab memory!\n"
              "If using -L or other prealloc options, max memory must be "
              "at least %d megabytes.\n",
              power_largest);
      exit(1);
    }
  }
}

void* SlabAllocator::memory_allocate(size_t size) {
  void *ret = NULL;

  if (mem_base == NULL) {
    /* We are not using a preallocated large memory chunk */
    epicLog(LOG_FATAL, "allocator is not initialized!");
  } else {
    ret = mem_current;

    if (size > mem_avail) {
      return NULL;
    }

    /* mem_current pointer _must_ be aligned!!! */
    if (size % CHUNK_ALIGN_BYTES) {
      size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
    }

    mem_current += size;
    if (size < mem_avail) {
      mem_avail -= size;
    } else {
      mem_avail = 0;
    }
  }
  return ret;
}
/*SlabAllocator类中的一个辅助函数，用于扩展指定Slab类的页面列表slab_list。主要功能包括：
1.检查当前Slab类的页面列表容量是否足够。
2.如果容量不足，则动态扩展页面列表的大小。
3.确保Slab类能够存储更多的内存页面地址。
参数：id——指定的Slab类ID，表示需要扩展页面列表的Slab类。
返回值：1——成功；0——失败。*/
int SlabAllocator::grow_slab_list(const unsigned int id) {
  slabclass_t *p = &slabclass[id]; //获取指定的Slab类描述符，以便检查和扩展其页面列表 
  //检查页面列表容量是否足够
  if (p->slabs == p->list_size) { //检查当前Slab类的页面计数p->slabs，是否已经达到页面列表的容量p->list_size。如果页面计数等于列表容量，说明页面列表已满，需要扩展
    //计算新的页面列表的大小
    size_t new_size = (p->list_size != 0) ? p->list_size * 2 : 16; //如果当前页面列表的容量不为0，将其容量翻倍。否则，初始化容量为16
    void *new_list = realloc(p->slab_list, new_size * sizeof(void *)); //使用realloc动态扩展页面列表的大小。新的列表大小为new_size * sizeof(void *)，即新的页面列表的容量乘以每个页面地址的大小 
    if (new_list == 0) //检查扩展是否成功，如果realloc返回0，表示扩展失败，直接返回0
      return 0;
    p->list_size = new_size; //更新页面列表容量为new_size
    p->slab_list = (void **) new_list; //更新页面列表的指针为new_list
  }
  return 1; //返回
}
/*split_slab_page_into_freelist函数是SlabAllocator类中的一个辅助函数，用于将分配的Slab页面拆分为多个内存块，并将这些块加入空闲列表。
参数：ptr——指向分配的Slab页面起始地址。id——指定的Slab类ID，表示将内存块加入哪个Slab类的空闲列表*/
void SlabAllocator::split_slab_page_into_freelist(char *ptr, const unsigned int id) {
  slabclass_t *p = &slabclass[id]; //获取指定Slab类描述符，以便操作该Slab类的空闲列表 
  int x;
  /*遍历页面中的每个内存块，并将其加入空闲列表。
  p->perslab表示当前Slab类中每个页面包含的内存块数量。p->size表示当前Slab类中每个内存块的大小。*/
  for (x = 0; x < p->perslab; x++) { 
    do_slabs_free(ptr, 0, id);//调用do_slabs_free函数将当前内存块(ptr)加入空闲列表
    ptr += p->size; //将指针ptr向后移动一个内存块的大小，以便处理下一个内存块
  }
}
/*do_slabs_newslab用于为指定的Slab类分配新的Slab页面。主要功能包括：
1.为指定的Slab类分配一块新的内存页面。
2.将分配的内存页面拆分为多个内存块，并加入空闲列表。
3.更新Slab类的内存分配统计信息。
参数：id——指定的Slab类ID，表示为哪个Slab类分配新的内存页面。
返回值：1——表示分配成功；0——表示分配失败。*/
int SlabAllocator::do_slabs_newslab(const unsigned int id) {
  slabclass_t *p = &slabclass[id]; //获取指定的Slab类描述符，以便为该Slab类分配新的内存页面
  //计算需要分配的内存页面大小(len)
  int len = slab_reassign ? item_size_max : p->size * p->perslab;//如果启用了slab_reassign，则分配的内存页面大小为item_size_max，否则为p->size * p->perslab ，即每个块的大小乘以每页的块数
  char *ptr;
  //检查内存分配限制和分配条件
  if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0)//内存限制；如果启用内存限制mem_limit，且当前已分配的内存(mem_malloced)加上新分配的内存(len)超过了限制，则分配失败；如果当前Slab类没有任何页面(p->slabs==0)，则允许分配。
      || (grow_slab_list(id) == 0) //扩展Slab列表：调用grow_slab_list函数扩展Slab列表的容量。如果失败，则返回0
      || ((ptr = (char *) memory_allocate((size_t) len)) == 0)) {//分配内存：分配新的内存页面。如果失败，则返回0

    epicLog(LOG_WARNING, "new slab class %d failed", id); //如果任意条件不满足，记录日志并返回0，表示分配失败
    return 0;
  }

  //FIXME (zh): check whether this is necessary
  //memset(ptr, 0, (size_t)len); //sep
  split_slab_page_into_freelist(ptr, id); //将分配的内存页面拆分为多个内存块，并将这些块加入当前Slab类的空闲列表
  //更新Slab类的统计信息
  p->slab_list[p->slabs++] = ptr; //将新分配的内存页面地址(ptr)加入Slab类的slab_list数组中，并增加Slab类的页面计数(p->slabs++)
  mem_malloced += len; //更新全局已分配内存统计信息（mem_malloced），表示当前已分配的内存总量
  MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id); //调用宏函数记录Slab类分配事件。

  return 1;
}

/*@null@*/
/*do_slabs_alloc是SlabAllocator类中用于从指定的Slab类分配内存块的核心函数。它的主要功能包括：
1.从指定的Slab类中分配内存块。
2.检查空闲列表是否有可用的内存块。
3.在空闲列表为空时，尝试分配新的Slab页面。
4.更新内存分配的统计信息。
参数；size——需要分配的内存大小(以字节为单位)。id——指定Slab类ID，表示从哪个Slab类中分配内存。
返回值：返回分配的内存地址(void *类型)。如果分配失败，返回NULL。*/
void * SlabAllocator::do_slabs_alloc(const size_t size, unsigned int id) {
  slabclass_t *p;
  void *ret = NULL;
  item *it = NULL;
  //检查Slab类ID是否在有效范围内
  if (id < POWER_SMALLEST || id > power_largest) {
    MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0); //如果无效，记录分配失败的日志并返回NULL
    return NULL;
  }

  p = &slabclass[id]; //获取指定Slab类，以便从该Slab类中分配内存 
#ifdef FINE_SLAB_LOCK //未定义
  p->lock();
#endif

  //printf("sl_curr = %d, ((item *)p->slots)->slabs_clsid = %d\n", p->sl_curr, ((item *)p->slots)->slabs_clsid);
  assert(p->sl_curr == 0 || ((item * )p->slots)->slabs_clsid == 0);

  /* fail unless we have space at the end of a recently allocated page,
   we have something on our freelist, or we could allocate a new page */
  //检查当前Slab类是否有可用的内存块
  //如果p->sl_curr != 0，表示当前Slab类有可用的内存块
  //如果没有空闲内存块，调用do_slabs_newslab(id)尝试分配新的Slab页面 
  if (!(p->sl_curr != 0 || do_slabs_newslab(id) != 0)) {
    /* We don't have more memory available */
    ret = NULL;  //如果没有可用的内存块且无法分配新的Slab页面，则返回NULL 
  } else if (p->sl_curr != 0) {  //从空闲列表中分配内存块
    /* return off our freelist */
    it = (item *) p->slots; //从当前Slab类的空闲列表p->slots中获取一个内存块
    p->slots = it->next; //更新空闲列表的头指针(p->slots)为下一个内存块 
    if (it->next)
      it->next->prev = 0;
    //hack by zh
    it->size = size; //设置分配的内存块的大小it->size
    it->slabs_clsid = id; //设置分配的内存块的Slab类ID it->slabs_clsid

    p->sl_curr--;  //减少空闲块计数
    //ret = (void *)it; //sep
    ret = it->data;  //sep  设置返回值为分配的内存块的地址
  }
#ifdef FINE_SLAB_LOCK
  p->unlock();
#endif
  //如果分配成功，更新统计信息
  if (ret) {
    p->requested += size;//更新Slab类的已分配内存统计信息
    MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    mem_free -= p->size; //更新全局的空闲内存统计信息， mem_free是全局空闲内存还是当前节点的空闲内存
  } else { 
    MEMCACHED_SLABS_ALLOCATE_FAILED(size, id); //如果分配失败，记录分配失败的日志
  }

  return ret;
}
/*SlabAllocator类中的一个核心函数，用于释放指定的内存块，并将其重新加入到指定Slab类的空闲列表中。它的主要功能包括：
1.检查内存块的合法性。
2.将内存块标记为已释放。
3.将内存块加入指定Slab类的空闲列表。
4.更新内存释放的统计信息。
参数：ptr——指向要释放的内存块的指针。size——要释放的内存块的大小(以字节为单位)。id——指定的Slab类ID，表示将内存块加入哪个Slab类的空闲列表。*/
void SlabAllocator::do_slabs_free(void *ptr, const size_t size,
                                  unsigned int id) {
  slabclass_t *p;
  item *it;

  //assert(((item *)ptr)->slabs_clsid == 0); //sep
  assert(id >= POWER_SMALLEST && id <= power_largest);//检查Slab类ID的合法性
  if (id < POWER_SMALLEST || id > power_largest) //确保指定的slab类ID在有效范围内
    return; //如果ID不合法，直接返回，不执行后续操作

  MEMCACHED_SLABS_FREE(size, id, ptr); //调用MEMCACHED_SLABS_FREE宏函数记录内存释放事件 
  p = &slabclass[id]; //获取指定Slab类描述符，以便操作该Slab类的空闲列表

  //it = (item *)ptr; //sep
  if (stats_map.count(ptr)) { //检查stats_map中是否已存在内存块的元数据
    it = stats_map.at(ptr);  //如果存在，，直接获取元数据
  } else {
    it = new item(); //如果不存在，创建新的item对象，并将其与内存块地址关联
    stats_map[ptr] = it;
  }
  it->data = ptr;  //sep

  it->it_flags |= ITEM_SLABBED;//标记内存块的标志位，表示该内存块已释放并可重用
  it->prev = 0;
  it->next = (struct _stritem *) p->slots; //将内存块插入到当前Slab类的空闲列表
  if (it->next)
    it->next->prev = it; 
  p->slots = it; //更新空闲列表的头指针为当前内存块

  p->sl_curr++; //增加当前Slab类的空闲块计数 
  p->requested -= size; //减少当前Slab类的已分配内存统计信息 
  if (size) //如果size不为零
    mem_free += p->size; //增加全局的空闲内存统计信息 
  return;
}

/*
 * return the ptr where data should be stored without the header
 */
void * SlabAllocator::sb_malloc(size_t size) {

#ifdef DHT
	/*
	 * enable to allocate memory larger than 1M
	 * FIXME: not support free of large block for now
	 */
	if(size > item_size_max) {
		lock();
		void* ret = memory_allocate(size);
		epicLog(LOG_WARNING, "allocate memory %lu, larger than default max %d, at %lx", size, item_size_max, ret);
		epicAssert(((uint64_t)ret % BLOCK_SIZE) == 0);
		bigblock_map[ret] = size;
		unlock();
		return ret;
	}
#endif

  lock();
  /*
   * if the slab-allocator isn't initiated, we use the default malloc()!
   */
  if (mem_limit == 0) {
    dbprintf("sb_mallocator is not initiated. Use default malloc\n");
    return NULL;
  }

  size_t newsize = size + SB_PREFIX_SIZE;

  unsigned int id = slabs_clsid(newsize);
  //item * ret = (item *)slabs_alloc(newsize, id); //sep
  //return ret == NULL ? NULL : ITEM_key(ret); //sep
  void* ret = slabs_alloc(newsize, id);  //sep
  epicAssert(ret);
  unlock();
  return ret;
}
/*sb_aligned_malloc是SlabAllocator类中的一个内存分配函数，用于分配对齐的内存块。它的主要功能包括：
1.分配指定大小的内存块，并确保内存地址对齐到指定的边界(block)
2.支持分配大于默认最大块大小(item_size_max)的内存(如果启用了DHT宏)
3.通过SLab分配器管理内存，减少内存碎片并提高分配效率
参数：
1.size:需要分配的内存大小（以字节为单位）
2.block:对齐的边界大小（以字节为单位）
返回值：
1.成功时返回分配的内存地址(void* 类型)，失败时返回NULL
*/
void * SlabAllocator::sb_aligned_malloc(size_t size, size_t block) {

#ifdef DHT
	/*
	 * enable to allocate memory larger than 1M
	 * FIXME: not support free of large block for now
	 */
	if(size > item_size_max) {
		epicLog(LOG_WARNING, "allocate memory %lu, larger than default max %lu", size, item_size_max);
		lock();
		void* ret = memory_allocate(size);
		epicAssert((uint64_t)ret % BLOCK_SIZE == 0);
		bigblock_map[ret] = size;
		unlock();
		return ret;
	}
#endif

  lock();
  /*
   * if the slab-allocator isn't initiated, we use the default malloc()!
   */
  //检查slab分配器是否已初始化
  if (mem_limit == 0) {//如果mem_limit为0，表示slab分配器未初始化，直接返回NULL。否则，继续执行分配逻辑。
    dbprintf("sb_mallocator is not initiated. Use default malloc\n");
    return NULL;
  }
  //调整内存大小并对齐
  size_t newsize = size + SB_PREFIX_SIZE; //将请求的内存大小size加上前缀大小SB_PREFIX_SIZE，得到新的内存大小newsize 

  newsize = ALIGN(newsize, block);//使用ALIGN宏将newsize对齐到指定的块大小block的倍数。
  //确定Slab类ID
  unsigned int id = slabs_clsid(newsize); //根据新的内存大小newsize计算Slab类ID，slabs_clsid函数返回对应的Slab类ID。每个Slab类对应不同大小的内存块
  //item * ret = (item *)slabs_alloc(newsize, id); //sep
  //return ret == NULL ? NULL : ITEM_key(ret); //sep
  void* ret = slabs_alloc(newsize, id);  //调用slabs_alloc函数，从指定的Slab类中分配内存，
  epicAssert(ret);//如果分配失败，epicAssert(ret)会触发断言，表示分配失败。分配成功后，ret指向分配的内存块。 
  epicLog(LOG_DEBUG, "ret = %lx, newsize = %d", ret, newsize);
  epicAssert((uint64_t )ret % block == 0); //确保分配的内存地址对齐到指定的块大小block的倍数 
  unlock(); //解锁分配器，允许其他线程进行内存分配
  return ret; //返回分配的内存地址
}

/*
 * return the ptr where data should be stored without the header
 */
void * SlabAllocator::sb_calloc(size_t count, size_t size) {

  dbprintf("sb_calloc size = %ld\n", size);

  if (unlikely(mem_limit == 0)) {
    dbprintf("using default calloc\n");
    return NULL;
  }

  void * ptr = sb_malloc(count * size);
  if (ptr != NULL) {
    epicLog(LOG_INFO,
            "WARNING: touch the registered memory area during allocation!!!");
    memset(ptr, 0, size);
  } else {
      epicAssert(false);
  }
  return ptr;
}

void * SlabAllocator::sb_aligned_calloc(size_t count, size_t size,
                                        size_t block) {

  dbprintf("sb_calloc size = %ld\n", size);

  if (unlikely(mem_limit == 0)) {
    dbprintf("using default calloc\n");
    return NULL;
  }

  void * ptr = sb_aligned_malloc(count * size, block);
  if (ptr != NULL) {
    epicLog(LOG_INFO,
            "WARNING: touch the registered memory area during allocation!!!");
    memset(ptr, 0, size);
  } else {
    epicLog(LOG_WARNING, "no free memory");
    epicAssert(false);
  }
  return ptr;
}

/*
 * return the ptr where data should be stored without the header
 */
void *SlabAllocator::sb_realloc(void * ptr, size_t size) {

  dbprintf("sb_realloc size = %ld\n", size);
  /*
   * if the slab-allocator isn't initiated, we use the default realloc()!
   */
  if (unlikely(mem_limit == 0)) {
    dbprintf("using default realloc\n");
    return NULL;
  }

  if (ptr == NULL)
    return sb_malloc(size);

  lock();
  //item * it1 = (item *) ((char*)ptr-SB_PREFIX_SIZE); //sep
  epicAssert(stats_map.count(ptr));  //sep
  item* it1 = stats_map.at(ptr);  //sep
  unsigned int id1 = it1->slabs_clsid;
  int size1 = it1->size;
  epicAssert(id1 == slabs_clsid(size1));

  size_t size2 = size + SB_PREFIX_SIZE;
  unsigned int id2 = slabs_clsid(size2);
  void* ret = nullptr;
  if (id1 == id2) {
    it1->size = size2;
    slabs_adjust_mem_requested(id1, size1, size2);
    ret = ptr;
  } else {
    epicAssert(size1 != size2);
    //item * it2 = (item *)slabs_alloc(size2, id2); //sep
    void* ptr = slabs_alloc(size2, id2);  //sep
    epicAssert(stats_map.count(ptr));  //sep
    item* it2 = stats_map.at(ptr);  //sep

    if (size2 < size1)
      memcpy(ITEM_key(it2), ptr, size);
    else
      memcpy(ITEM_key(it2), ptr, size1 - SB_PREFIX_SIZE);

    //need to clear the clsid as the original memcached implementation check this in free function
    it1->slabs_clsid = 0;
    //slabs_free(it1, size1, id1); //sep
    slabs_free(it1->data, size1, id1);  //sep
    ret = ITEM_key(it2);
  }
  unlock();
  epicAssert(ret);
  return ret;
}

bool SlabAllocator::is_free(void* ptr) {
  lock();
  epicAssert(stats_map.count(ptr));  //sep
  item* it = stats_map.at(ptr);  //sep
  bool ret = it->slabs_clsid == 0 ? true : false;
  unlock();
  return ret;
}

size_t SlabAllocator::get_size(void* ptr) {
  epicAssert(stats_map.count(ptr));
  item* it = stats_map[ptr];
  return it->size;
}

size_t SlabAllocator::sb_free(void *ptr) {
  lock();

#ifdef DHT
	if(bigblock_map.count(ptr)) {
		epicLog(LOG_WARNING, "not support free of big block for now");
		unlock();
		return 0;
	}
#endif

  /*
   * if the slab-allocator isn't initiated, we use the default free()!
   */
  if (mem_limit == 0) {
    epicLog(LOG_DEBUG, "allocator is not initialized");
    return 0;
  }

  //item * it = (item *) ((char*)ptr-SB_PREFIX_SIZE); //sep
  epicAssert(stats_map.count(ptr));  //sep
  item* it = stats_map.at(ptr);  //sep
  unsigned int id = it->slabs_clsid;
  size_t size = it->size;

  assert(id == slabs_clsid(it->size));
  it->slabs_clsid = 0;
  it->size = 0;
  //slabs_free(it, it->size, id); //sep
  //FIXME: remove below
  memset(it->data, 0, size);
  slabs_free(it->data, size, id);
  unlock();
  return size;
}
/*用于从指定的Slab类中分配内存块。
参数：需要分配的内存大小size；指定的Slab类ID，表示从哪个Slab类中分配内存*/
void *SlabAllocator::slabs_alloc(size_t size, unsigned int id) {
  void *ret;

  ////pthread_mutex_lock(&slabs_lock);
  ret = do_slabs_alloc(size, id);//调用底层的do_slabs_alloc函数完成实际的内存分配，是slabs_alloc函数的核心实现，负责从指定的Slab类中分配内存块
  ////pthread_mutex_unlock(&slabs_lock);
  return ret;
}

void SlabAllocator::slabs_free(void *ptr, size_t size, unsigned int id) {
  ////pthread_mutex_lock(&slabs_lock);
  slabclass_t* p = &slabclass[id];
#ifdef FINE_SLAB_LOCK
  p->lock();
#endif
  do_slabs_free(ptr, size, id);
#ifdef FINE_SLAB_LOCK
  p->unlock();
#endif
  ////pthread_mutex_unlock(&slabs_lock);
}

int SlabAllocator::nz_strcmp(int nzlength, const char *nz, const char *z) {
  int zlength = strlen(z);
  return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

void SlabAllocator::slabs_adjust_mem_requested(unsigned int id, size_t old,
                                               size_t ntotal) {
  ////pthread_mutex_lock(&slabs_lock);
  slabclass_t *p;
  if (id < POWER_SMALLEST || id > power_largest) {
    fprintf(stderr, "Internal error! Invalid slab class\n");
    abort();
  }

  p = &slabclass[id];
  p->requested = p->requested - old + ntotal;
  ////pthread_mutex_unlock(&slabs_lock);
}

SlabAllocator::~SlabAllocator() {
  if (mem_base)
    mmap_free(mem_base);
}
