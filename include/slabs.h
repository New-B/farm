/* slabs memory allocation */
#ifndef SLABS_H
#define SLABS_H
//文件定义了一个用于内存分配的SlabAllocator类及其相关数据结构和方法
//这些头文件提供了互斥锁、无序映射、标准整数类型和项目中其他模块的功能
#include <mutex>
#include <unordered_map>
#include <cstdint>
#include "log.h"
#include "settings.h"
#include "hashtable.h"
#include "lockwrapper.h"

#define ITEM_SIZE_MAX (1024*1024) //定义了项目的最大大小

/* Slab sizing definitions. */
#define POWER_SMALLEST 1  //最小幂次  定义了最小的slab类
#define POWER_LARGEST  200  //最大幂次  定义了最大的slab类
#define CHUNK_ALIGN_BYTES 8 //块对齐字节数  定义了块的对齐字节数
#define MAX_NUMBER_OF_SLAB_CLASSES (POWER_LARGEST + 1)  //最大slab类数  定义了最大的slab类数
#define STAT_KEY_LEN 128  //统计键长度  定义了统计键的长度
#define STAT_VAL_LEN 128  //统计值长度  定义了统计值的长度

#define ITEM_SLABBED 4  //定义了项目被分配的标志
#define ITEM_LINKED 1 //定义了项目被链接的标志
#define ITEM_CAS 2  //定义了项目的CAS标志
//这些宏定义了用于内存分配和日志记录的宏
#define MEMCACHED_SLABS_ALLOCATE(arg0, arg1, arg2, arg3)  //定义了内存分配的宏
#define MEMCACHED_SLABS_ALLOCATE_ENABLED() (0)  //定义了内存分配是否启用的宏
#define MEMCACHED_SLABS_ALLOCATE_FAILED(arg0, arg1) //定义了内存分配失败的宏
#define MEMCACHED_SLABS_ALLOCATE_FAILED_ENABLED() (0) //定义了内存分配失败是否启用的宏
#define MEMCACHED_SLABS_FREE(arg0, arg1, arg2)  //定义了内存释放的宏
#define MEMCACHED_SLABS_FREE_ENABLED() (0)  //定义了内存释放是否启用的宏
#define MEMCACHED_SLABS_SLABCLASS_ALLOCATE(arg0)  //定义了slab类分配的宏
#define MEMCACHED_SLABS_SLABCLASS_ALLOCATE_ENABLED() (0)  //定义了slab类分配是否启用的宏
#define MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(arg0) //定义了slab类分配失败的宏
#define MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED_ENABLED() (0) //定义了slab类分配失败是否启用的宏

#define dbprintf(fmt, ...) _epicLog ((char*)__FILE__, (char*)__func__, __LINE__, LOG_DEBUG, fmt, ## __VA_ARGS__)
//#define ITEM_key(item) (((char*)&((item)->data)) \
//         + (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))
#define ITEM_key(item) ((item)->data)

typedef struct {
  unsigned int size; /* sizes of items */ //项目的大小
  unsigned int perslab; /* how many items per slab */ //每个slab中有多少项目

  void *slots; /* list of item ptrs */  //项目指针列表  指向项目指针的列表
  unsigned int sl_curr; /* total free items in list */  //列表中的空闲项目总数

  unsigned int slabs; /* how many slabs were allocated for this class */  //为这个类分配了多少个slab

  void **slab_list; /* array of slab pointers */  //slab指针数组  slab指针数组
  unsigned int list_size; /* size of prev array */  //前一个数组的大小

  unsigned int killing; /* index+1 of dying slab, or zero if none */  //死亡slab的索引+1，如果没有则为零
#ifdef FINE_SLAB_LOCK
  atomic<size_t> requested; /* The number of requested bytes */
  mutex lock_;  //互斥锁，用于保护slab分配器的访问
  inline void lock() {
    lock_.lock(); 
  }
  inline void unlock() {
    lock_.unlock();
  }
#else
  size_t requested; /* The number of requested bytes */
#endif
} slabclass_t;

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
  struct _stritem *next;  //下一个项目  指向下一个项目的指针
  struct _stritem *prev;  //前一个项目  指向前一个项目的指针
  int size; //项目大小  项目的大小
  uint8_t it_flags; /* ITEM_* above */  //项目标志  项目标志
  uint8_t slabs_clsid;/* which slab class we're in */ //我们所在的slab类
  void* data; //项目数据  项目数据
} item;

/*SlabAllocator类是一个内存分配器，专门用于高效地管理和分配内存块。它采用了slab分配算法，这种算法
常用于操作系统和高性能应用中，以减少内存碎片并提高内存分配和释放的效率。
SlabAllocator类通过预分配内存块(slabs)来管理内存。每个slab包含多个大小相同的内存块(chunks)，用于
分配和释放固定大小的对象。*/
class SlabAllocator {
  /* powers-of-N allocation structures */
  slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];  //slab类数组，每个元素表示一个slab类，包含多个大小相同的内存块
  size_t mem_limit = 0; //内存限制，表示最大可分配的内存大小
  size_t mem_malloced = 0;  //分配的内存，已分配的内存大小
  int power_largest;  //最大幂次  最大幂次  （GPT：最大的slab类）最大的slab类索引

  char *mem_base = NULL;  //基地址  内存基地址
#ifdef FINE_SLAB_LOCK
  atomic<char *> mem_current;  // = NULL; //当前内存  当前内存地址
  atomic<size_t> mem_avail; // = 0; //可用内存  可用内存大小
  atomic<size_t> mem_free;    // = 0; //空闲内存  空闲内存大小
#else
  char* mem_current = NULL; //当前内存  当前内存地址
  size_t mem_avail = 0; //可用内存  可用内存大小
  size_t mem_free;  // = 0; //空闲内存  空闲内存大小
#endif

  int SB_PREFIX_SIZE = 0;  //sizeof(item);  //项目前缀大小  项目前缀大小

#ifdef FINE_SLAB_LOCK
  HashTable<void*, item*> stats_map;  //统计映射，
#else
  unordered_map<void*, item*> stats_map;  //统计映射，用于记录内存分配的统计信息
#endif

#ifdef DHT
	/*
	 * FIXME: not support free for now
	 */
	unordered_map<void*, size_t> bigblock_map;    //大块映射，用于记录大块内存的分配情况
#endif

  //TODO: no init func
  int chunk_size = 48;  //块大小  块大小
  int item_size_max = ITEM_SIZE_MAX;  //项目最大大小  项目最大大小
  size_t maxbytes = 64 * 1024 * 1024;   //最大字节数  最大字节数
  bool slab_reassign = true;  //slab重新分配，是否允许slab重新分配

#ifndef FINE_SLAB_LOCK
  LockWrapper lock_;  //互斥锁，用于保护slab分配器的访问
#endif

  /**
   * Access to the slab allocator is protected by this lock
   */
  //static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;
  //static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;
  int nz_strcmp(int nzlength, const char *nz, const char *z); //比较两个字符串  比较字符串
  void do_slabs_free(void *ptr, const size_t size, unsigned int id);  //释放内存  释放slab
  void* do_slabs_alloc(const size_t size, unsigned int id); //分配内存  分配slab
  int do_slabs_newslab(const unsigned int id);  //创建新的slab  创建新的slab
  void split_slab_page_into_freelist(char *ptr, const unsigned int id); //将slab页面分割为空闲列表  将slab页面分割为空闲列表
  int grow_slab_list(const unsigned int id);  //增加slab列表  增加slab列表
  void* memory_allocate(size_t size); //分配内存  分配内存
  void slabs_preallocate(const unsigned int maxslabs);    //预分配slab  预分配slab
  void* mmap_malloc(size_t size); //分配内存  使用mmap分配内存
  void mmap_free(void* ptr);  //释放内存  使用mmap释放内存

  /** Allocate object of given length. 0 on error *//*@null@*/
  void *slabs_alloc(const size_t size, unsigned int id);  //分配内存  分配slab

  /** Free previously allocated object */
  void slabs_free(void *ptr, size_t size, unsigned int id); //释放内存  释放slab

  /** Adjust the stats for memory requested */
  void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal);  //调整内存请求的统计  调整内存请求的统计

  inline void lock() {  //加锁
#ifndef FINE_SLAB_LOCK
    lock_.lock();
#endif
  }

  inline void unlock() {  //解锁
#ifndef FINE_SLAB_LOCK
    lock_.unlock();
#endif
  }
  void slabs_destroy(); //销毁slab  销毁slab分配器

  /**
   * Given object size, return id to use when allocating/freeing memory for object
   * 0 means error: can't store such a large object
   */

  unsigned int slabs_clsid(const size_t size);  //返回slab类ID  根据对象大小返回slab类ID

 public:
  /** Init the subsystem. 1st argument is the limit on no. of bytes to allocate,
   0 if no limit. 2nd argument is the growth factor; each slab will use a chunk
   size equal to the previous slab's chunk size times this factor.
   3rd argument specifies if the slab allocator should allocate all memory
   up front (if true), or allocate memory in chunks as it is needed (if false)
   */
  void* slabs_init(const size_t limit, const double factor,
                   const bool prealloc);  //初始化slab  初始化slab分配器
  size_t get_avail(); //获取可用内存  获取可用内存

  void *sb_calloc(size_t count, size_t size); //分配内存  分配内存并初始化为0 分配并清零内存
  void *sb_malloc(size_t size); //分配内存  分配内存
  void* sb_aligned_malloc(size_t size, size_t block = BLOCK_SIZE);  //分配内存  分配对齐内存
  void* sb_aligned_calloc(size_t count, size_t size, size_t block = BLOCK_SIZE);  //分配内存  分配对齐内存并初始化为0
  void *sb_realloc(void * ptr, size_t size);  //重新分配内存  重新分配内存
  size_t sb_free(void * ptr);   //释放内存  释放内存
  bool is_free(void* ptr);  //检查是否释放  检查内存是否已释放
  size_t get_size(void* ptr); //获取内存大小  获取内存大小

  ~SlabAllocator();   //析构函数  销毁slab分配器

};

#endif
