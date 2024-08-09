#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<iostream>
#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<thread>
#include<future>
#include<functional>

const int TASK_MAX_THRESHHOLD = 2;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_TIME = 10;

enum class PoolMode {
	MODE_FIXED, //数量固定的线程
	MODE_CACHED,//线程数量可动态增长
};

//线程类型
class Thread {
public:
	using ThreadFunc = std::function<void(int)>;
	Thread(ThreadFunc func)
		: func_(func)
		, Threadid_(generateId_++)
	{}
	~Thread() = default;
	//启动线程
	void start()
	{
		std::thread t(func_, Threadid_);//创建线程t 和线程函数
		t.detach();
	}
	//获取线程id
	int getId() const
	{
		return Threadid_;
	}
private:
	ThreadFunc func_;//用来接收绑定的线程函数
	static int generateId_;//用来生成自定义线程池id，与std::this_thread::get_id()不同  静态成员变量要在类外进行赋值
	int Threadid_;//保存线程id 用来映射线程id对线程对象的映射
};

int Thread::generateId_ = 0;

//线程池类型 
class ThreadPooL {
public:
	//线程池构造
	ThreadPooL()
		: initThreadSize_(0)
		, taskSize_(0)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, curThreadSize_(0)
		, idleThreadSize_(0)
	{}
	//线程池析构
	~ThreadPooL()
	{
		isPoolRunning_ = false;
		// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();//放在加锁之后
		//等待所有线程执行完再析构
		exitcond_.wait(lock, [&]()->bool { return threads_.size() == 0; });// 当线程池中有线程时，则释放锁，由等待变为阻塞状态
	}
	//设置线程池的工作模式
	void setMode(PoolMode mode)
	{
		if (chekRunningState())
			return;
		poolMode_ = mode;
	}

	//设置Task任务队列上限阈值
	void setTaskMaxQueThreshHold(int threshhold)
	{
		if (chekRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	//设置线程池cached模式下线程的阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (chekRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshhold;
		}
	}

	//给线程池提交任务
	//Result submitTask(std::shared_ptr<Task> sp);
	// 给线程池提交任务
	// 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	// pool.submitTask(sum1, 10, 20);   csdn  大秦坑王  右值引用+引用折叠原理
	// 返回值future<>
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		//打包任务，放入任务队列
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>
			(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));//task为局部变量，出了submitTask作用域后，不用担心其失效和手动释放内存的问题
		std::future<RType> result = task->get_future();

		//先获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//线程通信 等待任务队列中有空余才能向里面提交任务
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < size_t(taskQueMaxThreshHold_); }))
			//wait_for返回值为bool类型，返回false则表示等待了1秒后依然不满足taskQue_.size() < size_t(taskQueMaxThreshHold_)
		{
			//进行用户提示
			std::cout << "task queue is full, submit fail..." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>
				([]()->RType { return RType(); });
				(*task)();
			return task->get_future();//返回0值
		}
		//表示有空余，将任务放入任务队列
		//using Task = std::function<void()>;
		taskQue_.emplace([task](){(*task)(); });
		taskSize_++;

		//因为加入了任务，因此通知所有等待取出任务的线程
		notEmpty_.notify_all();

		// cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_//这里必须是taskSize_，因为taskQue_.size()永远不会大于
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			//测试语句
			std::cout << ">>>create new thread" << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this, std::placeholders::_1));//make_unique会new一个thread对象
			//std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this));
			int Threadid = ptr->getId();
			threads_.emplace(Threadid, std::move(ptr));
			//创建完之后要启动线程
			threads_[Threadid]->start();
			//修改线程个数相关的变量
			curThreadSize_++;
			idleThreadSize_++;
		}
		return result;
	}

	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())//表示CPU的核心数量作为线程初始化数量
	{
		isPoolRunning_ = true;//start之后便不能再设置线程池工作模式

		//记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//创建线程对象：先统一创建好所有线程，然后一次性启动所有
		//创建thread线程对像的时候，把线程函数给到thread线程对象
		for (int i = 0; i < initThreadSize_; i++) {//
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this, std::placeholders::_1));//make_unique会new一个thread对象
			//std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this));
			int Threadid = ptr->getId();
			threads_.emplace(Threadid, std::move(ptr));//因为thread_是uniqueptr，禁止左值的拷贝构造，要用move
		}
		//启动所有线程 std::vector<std::shared_ptr<Thread>> Thread_
		for (int i = 0; i < initThreadSize_; i++) {
			threads_[i]->start();//需要去执行一个线程函数
			idleThreadSize_++;
		}
	}
	//禁止用户使用赋值和复制
	ThreadPooL(const ThreadPooL&) = delete;
	ThreadPooL& operator = (const ThreadPooL&) = delete;

private:
	void threadFunc(int threadid)
	{
		//获取当前精确时间
		auto lastTime = std::chrono::high_resolution_clock().now();//获取当前精确时间

		for (;;) //死循环，一个技师服务完一个客人，继续留在等候室抢客人，而不是跟着客人一起走
			//while(isPoolRunning_)//若先判断线程池是否运行状态，则可能还未执行任务，线程池就析构了
		{
			Task sp;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "尝试获取任务" << std::endl;

				// cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
				// 结束回收掉（超过initThreadSize_数量的线程要进行回收）
				// 当前时间 - 上一次线程执行的时间 > 60s

				//锁 + 双重判断
				while (0 == taskQue_.size())
				{
					if (!isPoolRunning_)
					{
						//主线程结束，线程池回收，回收里面的线程
						threads_.erase(threadid);
						std::cout << "threadid：" << std::this_thread::get_id() << "exit"
							<< std::endl;
						exitcond_.notify_all();//notify之后使wait状态转为阻塞状态，抢到锁后进入就绪状态
						return;
					}
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//每一秒钟返回一次  怎么区分：超时返回？还是有任务待执行返回
							// 条件变量，超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))//没有任务时会一直阻塞在此处
						{
							auto now = std::chrono::high_resolution_clock().now();//high_resolution_clock()指的是获取当前精确时间
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
								// threadid => thread对象 => 删除
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid：" << std::this_thread::get_id() << "exit"//只exit空闲的线程
									<< std::endl;
								return;
							}
						}
					}
					else//poolMode == PoolMode::MODE_FIXED
					{
						//等待不空时取出
						notEmpty_.wait(lock);//相当于把条件给提出来，当notify_all 了 notEmpty 时，判断while(taskQue_.size() == 0) 中是否大于0，大于则跳出while循环
						//notEmpty_.wait(lock, [&]()->bool { return taskQue_.size() > 0; });
					}
				}
				idleThreadSize_--;
				//线程池要结束，回收资源
				//if (!isPoolRunning_)
				//{
				//	break;//与第二个while同级别，当上面wait的条件变量被唤醒后，再转一圈后，发现isPoolRunning_被置为false，则再次判断，跳出
				//	//大循环，直接对线程列表中的线程进行删除，相当于优化代码，将析构线程池中两种情况用一块代码来处理
				//}
				std::cout << "tid:" << std::this_thread::get_id()
					<< "获取任务成功" << std::endl;

				//表示任务队列不空，开始取出任务
				// 从任务队列种取一个任务出来，“取出的任务”就是submitTask的那个任务！！！
				sp = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				//如果还有任务，则通知其他空闲的线程执行任务，不能让线程空闲下来
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}
				//容器不满，通知可以继续向里面提交任务
				notFull_.notify_all();//notify_all之后要立即释放锁，不然就算notify_all了，submitTask线程也无法启动
			}//出了作用域，释放掉锁，不然不放掉锁的话，线程池每次只能一个线程抢到任务，做完任务后才释放，达成不了线程池的作用

			//这个线程来执行用户自定义的任务（执行的指令）
			if (nullptr != sp) {
				sp();
			}
			//事情做完了
			idleThreadSize_++;
			auto lastTime = std::chrono::high_resolution_clock().now();//更新当前精确时间
		}
	}
	bool chekRunningState() const
	{
		return isPoolRunning_;
	}

private:
	//Thread是通过new出来的，一旦new的话就需要delete掉，使用shared_ptr智能指针就不用手动析构了
	//std::vector<Thread*> Threads_;//线程列表
	//std::vector<std::unique_ptr<Thread>> threads_;//线程列表：threadpool.cpp中的auto ptr 就是threads_
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//做成map，映射线程id到线程对象的映射

	int initThreadSize_;//初始线程数量
	int threadSizeThreshHold_;//线程数量上限阈值
	std::atomic_int curThreadSize_;//记录当前线程池里面线程的总数量，也可以使用map中的 .size方法
	std::atomic_int idleThreadSize_;//记录线程池中空闲线程的数量

	//Task任务 -》 函数对象 不用担心用户将任务设为局部，生命周期的问题
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;
	std::atomic_int taskSize_;//任务数量  多个线程对任务数量进行++--,难免会出现线程安全问题，加锁的话会造成额外的消耗，submitTask和ThreadFunc线程
	int taskQueMaxThreshHold_;//任务队列数量上限阈值

	std::mutex taskQueMtx_; //保证任务队列的线程安全  应用在submitTask和ThreadFunc和main主线程(ThreadFunc的析构函数)
	//两个条件变量，当任务队列不满时，用户线程可以向里面提交线程，当任务队列不空时，线程池内部的线程可以向里面
	//取出任务进行消费。
	std::condition_variable notFull_;//表示任务队列不满
	std::condition_variable notEmpty_;//表示任务队列不空
	std::condition_variable exitcond_;//表示等待线程资源全部回收

	PoolMode poolMode_; //当前线程池的工作模式
	std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态  

};
#endif