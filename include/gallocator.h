// Copyright (c) 2018 The GAM Authors 


#ifndef INCLUDE_GALLOCATOR_H_
#define INCLUDE_GALLOCATOR_H_

#include <cstring>
#include <cstdint>
#include "structure.h"
#include "worker.h"
#include "settings.h"
#include "worker_handle.h"
#include "master.h"
#include "farm.h"

/*GAlloc类的设计目的是提供一个接口，用于管理全局内存分配和事务操作。它包含了内存分配、读取、写入、释放，以及事务的开始、提交和中止等功能。
1.内存管理：提供了分配、读取、写入和释放内存的方法。
2.键值对存储和获取：提供了存储和获取键值对的方法。
3.事务管理：提供了开始、分配内存、释放内存、读取、写入、提交和中止事务的方法。*/
class GAlloc{
// the only class the app-layer is aware of
public:
	GAddr Malloc(const Size size, Flag flag = 0); //分配内存
	GAddr AlignedMalloc(const Size size, Flag flag = 0); //分配对齐的内存
    int Read(const GAddr addr, void* buf, const Size count, Flag flag = 0); //读取数据
	int Read(const GAddr addr, const Size offset, void* buf, const Size count, Flag flag = 0); //带偏移量读取数据
	int Write(const GAddr addr, void* buf, const Size count, Flag flag = 0);//写入数据
	int Write(const GAddr addr, const Size offset, void* buf, const Size count, Flag flag = 0);//带偏移量写入数据
	void Free(const GAddr addr);//释放内存

	Size Put(uint64_t key, const void* value, Size count); //存储键值对
	Size Get(uint64_t key, void* value);//获取键值对

	void txBegin(); //开始事务
    GAddr txAlloc(size_t size, GAddr a = 0); //分配内存事务
    void txFree(GAddr); //释放内存事务
    int txRead(GAddr, void*, osize_t); //事务读取
    int txRead(GAddr, const Size, void*, osize_t);//带偏移量的事务读取
    int txWrite(GAddr, void*, osize_t); //事务写入
    int txWrite(GAddr, const Size, void*, osize_t);//带偏移量的事务写入
    int txAbort();//中止事务
    int txCommit();//提交事务

   	int txKVGet(uint64_t key, void* value, int node_id); //事务获取键值对
   	int txKVPut(uint64_t key, const void* value, size_t count, int node_id); //事务存储键值对
   	int KVGet(uint64_t key, void *value, int node_id);//获取键值对
   	int KVPut(uint64_t key, const void* value, size_t count, int node_id); //存储键值对

    GAlloc(Worker* w) { //构造函数， 接受一个Worker指针，用于初始化Farm对象
        farm = new Farm(w); //初始化farm
    }

    ~GAlloc() {delete farm;}//析构函数，释放farm

protected://保护类型可以确保只有该类及其子类可以访问这些成员
	Farm *farm; //Farm对象的指针，用于管理事务和内存操作
};

class GAllocFactory {
	static const Conf* conf; //静态配置指针
	static Worker* worker; //静态worker指针
	static Master* master; //静态master指针
	static mutex lock; //静态互斥锁，静态成员变量，用于在多线程环墨中保护共享资源，确保线程安全
	/*线程同步：在多线程环境中，多个线程可能会同时访问或修改共享资源，使用lock可以确保在同一时间
	只有一个线程可以访问或修改这些共享资源，从而避免数据竞争和不一致性。
	保护共享资源：通过在访问共享资源的代码端前后加锁和解锁，可以确保这些代码段在执行时不会被其他线程打断，
	从而保护共享资源的完整性。*/
public:
	static void InitSystem(const std::string& conf_file) { //初始化系统
		InitSystem(ParseConf(conf_file)); //解析配置文件并初始化系统
	}
	static void InitSystem(const Conf* c = nullptr) { //初始化系统
		lock.lock(); //加锁，确保在同一时间只有一个线程可以进入这段代码，从而保护共享资源
		//主要用于保护对静态成员变量conf、worker和master的访问，确保线程安全
		if(c) {  //如果传入了非空的conf,则设置配置
			if(!conf) {	//如果配置不存在,即类的共享静态变量conf尚未被初始化
				conf = c;//设置配置
			} else {
				epicLog(LOG_INFO, "NOTICE: Conf already exist %lx", conf); //记录日志
			}
		} else {//如果没有传入conf
			if(!conf) {//而且类的共享静态变量conf也未被初始化
				epicLog(LOG_FATAL, "Must provide conf for the first time"); //记录致命错误日志
			}
		}

		if(conf->is_master) { //如果配置为主节点
			if(!master) master = MasterFactory::CreateServer(*conf); //创建master
		}
		if(!worker) { //如果worker不存在
			worker = WorkerFactory::CreateServer(*conf); //创建worker
		}
		lock.unlock(); //解锁
	}
	static void FinalizeSystem() { //结束系统
		lock.lock(); //加锁
		delete conf; //释放配置
		delete worker; //释放worker
		delete master; //释放master
		conf = nullptr; //配置指针为空
		worker = nullptr; //worker指针为空
		master = nullptr; //master指针为空
		lock.unlock(); //解锁
	}

	// 创建分配器
	static GAlloc* CreateAllocator() {
	//	lock.lock(); // 加锁
		GAlloc* ret = new GAlloc(worker);
	//	lock.unlock(); // 解锁
		return ret;
	}
/*
	//  this function should be call in every thread
	//  in order to init some per-thread data
	 
	static GAlloc* CreateAllocator(const std::string& conf_file) {//创建分配器
		return CreateAllocator(ParseConf(conf_file)); //解析配置文件并创建分配器
	}
*/
	static const Conf* InitConf() { //初始化配置
		lock.lock(); //加锁
		conf = new Conf(); //创建新配置
		const Conf* ret = conf; //返回配置指针
		lock.unlock(); //解锁
		return ret; //返回配置指针
	}

	// need to call for every thread
	// c是一个指向Conf对象的常量指针，const关键字表示通过该指针不能修改指向的Conf对象。
	// =nullptr是默认参数值，表示如果调用函数时没有传递该参数，则指针c默认为空指针
	// 当将&conf作为实际参数传递给一个接受const Conf*类型的形式参数时，实际上传递的是
	// conf对象的地址，并且在函数内不能修改conf对象的内容
	// static GAlloc* CreateAllocator(const Conf* c = nullptr) { //创建分配器
	// 	lock.lock(); //加锁，确保在同一时间只有一个线程可以进入这段代码，从而保护共享资源
	// 	//主要用于保护对静态成员变量conf、worker和master的访问，确保线程安全
	// 	if(c) {  //如果传入了非空的conf,则设置配置
	// 		if(!conf) {	//如果配置不存在,即类的共享静态变量conf尚未被初始化
	// 			conf = c;//设置配置
	// 		} else {
	// 			epicLog(LOG_INFO, "NOTICE: Conf already exist %lx", conf); //记录日志
	// 		}
	// 	} else {//如果没有传入conf
	// 		if(!conf) {//而且类的共享静态变量conf也未被初始化
	// 			epicLog(LOG_FATAL, "Must provide conf for the first time"); //记录致命错误日志
	// 		}
	// 	}

	// 	if(conf->is_master) { //如果需要将本机配置为主节点master //	//而且如果静态成员变量master的指针没有被初始化
	// 		if(!master) master = MasterFactory::CreateServer(*conf); //创建master
	// 		// 对于const Conf* GAllocFactory::conf = nullptr; *conf是一个解引用操作，
	// 		// 表示conf指针所指向的Conf对象.conf是一个指向Conf对象的指针，类型为const Conf*。
	// 		// *conf的类型是const Conf&，即一个对Conf对象的常量引用，是Conf对象本身。
	// 	}
	// 	if(!worker) {//master同时也需要当做worker存在
	// 		worker = WorkerFactory::CreateServer(*conf); //创建worker
	// 	}
	// 	GAlloc* ret = new GAlloc(worker); //创建Galloc对象
	// 	lock.unlock();//解锁
	// 	return ret;//返回GAlloc对象指针
	// }

/*
	//need to call for every thread
	static GAlloc* CreateAllocator(const Conf& c) { //创建分配器
		lock.lock(); //加锁
		if(!conf) { //如果配置不存在
			Conf* lc = new Conf(); //创建新配置
			*lc = c; //复制配置
			conf = lc; //设置配置
		} else {
			epicLog(LOG_INFO, "Conf already exist %lx", conf); //记录日志
		}
		if(conf->is_master) {
			if(!master) master = MasterFactory::CreateServer(*conf); //创建master 
		}
		if(!worker) {
			worker = WorkerFactory::CreateServer(*conf); //创建worker
		}
		GAlloc* ret = new GAlloc(worker); //创建GAlloc对象
		lock.unlock(); //解锁
		return ret; //返回GAlloc对象指针
	}
*/
	static void SetConf(Conf* c) {//设置配置
		lock.lock(); //加锁
		conf = c; //设置配置
		lock.unlock(); //解锁
	}

	static void FreeResouce() { //释放资源
		lock.lock(); //加锁
		delete conf; //释放配置
		delete worker; //释放worker
		delete master; //释放master
		lock.unlock(); //解锁
	}

	/*
	 * TODO: fake parseconf function***********************************************************很明显是一个空壳函数，没有实现任何功能
	 */
	static const Conf* ParseConf(const std::string& conf_file) { //解析配置文件
		Conf* c = new Conf(); //创建新配置
		return c; //返回配置文件
	}

	static int LogLevel() {
		int ret = conf->loglevel; //获取日志级别
		return ret; //返回日志级别
	}

	static string* LogFile() { //获取日志文件
		string* ret = conf->logfile; //获取日志文件
		return ret; //返回日志文件
	}
};

#endif /* INCLUDE_GALLOCATOR_H_ */
