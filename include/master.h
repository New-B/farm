// Copyright (c) 2018 The GAM Authors 
//文件定义了Master类及其相关的工厂类MasterFactory，
#ifndef INCLUDE_MASTER_H_
#define INCLUDE_MASTER_H_

#include <unordered_map>
#include <queue>
#include "ae.h"
#include "settings.h"
#include "log.h"
#include "server.h"

class Master: public Server {	//Master类继承自Server类，表示主节点服务器
	//the service thread
	thread* st;	//服务线程	

	//worker list: ip:port,ip:port
	string worker_ips;	//工作节点列表，格式为“ip:port,ip:port”	

	//worker counter
	int workers;	//工作节点计数器

	queue<Client*> unsynced_workers;	//未同步的工作节点队列
	unordered_map<uint64_t, pair<void*, Size>> kvs;	//键值对存储

	unordered_map<uint64_t, queue<pair<Client*, WorkRequest*>>> to_serve_kv_request;	//待服务的键值对请求队列

public:
	Master(const Conf& conf);	//构造函数，初始化主节点服务器
	inline void Join() {st->join();}	//等待服务线程结束

	inline bool IsMaster() {return true;}	//判断是否为主节点，返回true
	inline int GetWorkerId() {return 0;}	//获取工作节点ID，主节点的ID为0

	void Broadcast(const char* buf, size_t len);	//广播消息给所有工作节点

  void FarmProcessRemoteRequest(Client* client, const char* msg, uint32_t size);	//处理远程请求
  void FarmResumeTxn(Client*){}	//恢复事务(未实现)

    void ProcessRequest(Client* client, WorkRequest* wr);	//处理客户端请求
    //some post process after accepting a TCP connection (e.g., send the worker list)
	int PostAcceptWorker(int, void*);	//接受工作节点连接后的处理
	//inline int PostConnectMaster(int fd, void* data) {return 0;} //not used

	~Master();	//析构函数，释放资源
};

class MasterFactory {	//MasterFactory类，用于创建和管理主节点服务器Master
	static Master *server;	//主节点服务器指针，静态指针，指向唯一的Master实例
public:
	static Server* GetServer() {	//获取Master主节点服务器的实例，如果不存在则抛出异常
		if (server)
			return server;
		else
			throw SERVER_NOT_EXIST_EXCEPTION;
	}
	static Master* CreateServer(const Conf& conf) {	//创建Master主节点服务器的实例，如果已存在则抛出异常
	/*const Conf& conf是一个对Conf对象的常量引用，通过引用传递参数，可以避免拷贝对象，提高效率。
	const关键字表示通过该引用不能修改引用的Conf对象。
	当将*conf作为实际参数传递给一个接收const Conf&类型的形式参数时，实际上传递的是conf指针所指向的Conf对象的引用，
	并且在函数内部不能修改改对象的内容。*/
		if (server)
			throw SERVER_ALREADY_EXIST_EXCEPTION;
		server = new Master(conf);
		return server;
	}
	~MasterFactory() {	//析构函数，删除Master主节点服务器的实例
		if (server)
			delete server;
	}
};

#endif /* INCLUDE_MASTER_H_ */

/*
对象本身：
定义：直接定义一个对象
存储：对象本身占用内存空间，存储对象的所有数据。
访问：通过对象名直接访问其成员。

对象的引用：
定义：引用是对象的别名，必须在定义时初始化，且不能改变引用的对象。
存储：引用本身不占用额外的内存空间，只是对象的一个别名。
访问：通过引用名访问对象的成员，与通过对象名访问相同。
特点：引用不能为null，且一旦绑定到一个对象，就不能再绑定到其他对象。

对象的指针：
定义：指针是存储对象地址的变量，可以在定义后指向不同的对象。
存储：指针本身占用内存空间，存储对象的地址。
访问：通过解引用操作符*访问对象的成员。
特点：指针可以为null，可以动态改变指向的对象。
*/
