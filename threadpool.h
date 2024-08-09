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
	MODE_FIXED, //�����̶����߳�
	MODE_CACHED,//�߳������ɶ�̬����
};

//�߳�����
class Thread {
public:
	using ThreadFunc = std::function<void(int)>;
	Thread(ThreadFunc func)
		: func_(func)
		, Threadid_(generateId_++)
	{}
	~Thread() = default;
	//�����߳�
	void start()
	{
		std::thread t(func_, Threadid_);//�����߳�t ���̺߳���
		t.detach();
	}
	//��ȡ�߳�id
	int getId() const
	{
		return Threadid_;
	}
private:
	ThreadFunc func_;//�������հ󶨵��̺߳���
	static int generateId_;//���������Զ����̳߳�id����std::this_thread::get_id()��ͬ  ��̬��Ա����Ҫ��������и�ֵ
	int Threadid_;//�����߳�id ����ӳ���߳�id���̶߳����ӳ��
};

int Thread::generateId_ = 0;

//�̳߳����� 
class ThreadPooL {
public:
	//�̳߳ع���
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
	//�̳߳�����
	~ThreadPooL()
	{
		isPoolRunning_ = false;
		// �ȴ��̳߳��������е��̷߳���  ������״̬������ & ����ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();//���ڼ���֮��
		//�ȴ������߳�ִ����������
		exitcond_.wait(lock, [&]()->bool { return threads_.size() == 0; });// ���̳߳������߳�ʱ�����ͷ������ɵȴ���Ϊ����״̬
	}
	//�����̳߳صĹ���ģʽ
	void setMode(PoolMode mode)
	{
		if (chekRunningState())
			return;
		poolMode_ = mode;
	}

	//����Task�������������ֵ
	void setTaskMaxQueThreshHold(int threshhold)
	{
		if (chekRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	//�����̳߳�cachedģʽ���̵߳���ֵ
	void setThreadSizeThreshHold(int threshhold)
	{
		if (chekRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshhold;
		}
	}

	//���̳߳��ύ����
	//Result submitTask(std::shared_ptr<Task> sp);
	// ���̳߳��ύ����
	// ʹ�ÿɱ��ģ���̣���submitTask���Խ������������������������Ĳ���
	// pool.submitTask(sum1, 10, 20);   csdn  ���ؿ���  ��ֵ����+�����۵�ԭ��
	// ����ֵfuture<>
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		//������񣬷����������
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>
			(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));//taskΪ�ֲ�����������submitTask������󣬲��õ�����ʧЧ���ֶ��ͷ��ڴ������
		std::future<RType> result = task->get_future();

		//�Ȼ�ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//�߳�ͨ�� �ȴ�����������п�������������ύ����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < size_t(taskQueMaxThreshHold_); }))
			//wait_for����ֵΪbool���ͣ�����false���ʾ�ȴ���1�����Ȼ������taskQue_.size() < size_t(taskQueMaxThreshHold_)
		{
			//�����û���ʾ
			std::cout << "task queue is full, submit fail..." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>
				([]()->RType { return RType(); });
				(*task)();
			return task->get_future();//����0ֵ
		}
		//��ʾ�п��࣬����������������
		//using Task = std::function<void()>;
		taskQue_.emplace([task](){(*task)(); });
		taskSize_++;

		//��Ϊ�������������֪ͨ���еȴ�ȡ��������߳�
		notEmpty_.notify_all();

		// cachedģʽ ������ȽϽ��� ������С��������� ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_//���������taskSize_����ΪtaskQue_.size()��Զ�������
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			//�������
			std::cout << ">>>create new thread" << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this, std::placeholders::_1));//make_unique��newһ��thread����
			//std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this));
			int Threadid = ptr->getId();
			threads_.emplace(Threadid, std::move(ptr));
			//������֮��Ҫ�����߳�
			threads_[Threadid]->start();
			//�޸��̸߳�����صı���
			curThreadSize_++;
			idleThreadSize_++;
		}
		return result;
	}

	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())//��ʾCPU�ĺ���������Ϊ�̳߳�ʼ������
	{
		isPoolRunning_ = true;//start֮��㲻���������̳߳ع���ģʽ

		//��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//�����̶߳�����ͳһ�����������̣߳�Ȼ��һ������������
		//����thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���
		for (int i = 0; i < initThreadSize_; i++) {//
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this, std::placeholders::_1));//make_unique��newһ��thread����
			//std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPooL::threadFunc, this));
			int Threadid = ptr->getId();
			threads_.emplace(Threadid, std::move(ptr));//��Ϊthread_��uniqueptr����ֹ��ֵ�Ŀ������죬Ҫ��move
		}
		//���������߳� std::vector<std::shared_ptr<Thread>> Thread_
		for (int i = 0; i < initThreadSize_; i++) {
			threads_[i]->start();//��Ҫȥִ��һ���̺߳���
			idleThreadSize_++;
		}
	}
	//��ֹ�û�ʹ�ø�ֵ�͸���
	ThreadPooL(const ThreadPooL&) = delete;
	ThreadPooL& operator = (const ThreadPooL&) = delete;

private:
	void threadFunc(int threadid)
	{
		//��ȡ��ǰ��ȷʱ��
		auto lastTime = std::chrono::high_resolution_clock().now();//��ȡ��ǰ��ȷʱ��

		for (;;) //��ѭ����һ����ʦ������һ�����ˣ��������ڵȺ��������ˣ������Ǹ��ſ���һ����
			//while(isPoolRunning_)//�����ж��̳߳��Ƿ�����״̬������ܻ�δִ�������̳߳ؾ�������
		{
			Task sp;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ����" << std::endl;

				// cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ�����߳�
				// �������յ�������initThreadSize_�������߳�Ҫ���л��գ�
				// ��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s

				//�� + ˫���ж�
				while (0 == taskQue_.size())
				{
					if (!isPoolRunning_)
					{
						//���߳̽������̳߳ػ��գ�����������߳�
						threads_.erase(threadid);
						std::cout << "threadid��" << std::this_thread::get_id() << "exit"
							<< std::endl;
						exitcond_.notify_all();//notify֮��ʹwait״̬תΪ����״̬����������������״̬
						return;
					}
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//ÿһ���ӷ���һ��  ��ô���֣���ʱ���أ������������ִ�з���
							// ������������ʱ������
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))//û������ʱ��һֱ�����ڴ˴�
						{
							auto now = std::chrono::high_resolution_clock().now();//high_resolution_clock()ָ���ǻ�ȡ��ǰ��ȷʱ��
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// ��ʼ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ�޸�
								// ���̶߳�����߳��б�������ɾ��   û�а취 threadFunc��=��thread����
								// threadid => thread���� => ɾ��
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid��" << std::this_thread::get_id() << "exit"//ֻexit���е��߳�
									<< std::endl;
								return;
							}
						}
					}
					else//poolMode == PoolMode::MODE_FIXED
					{
						//�ȴ�����ʱȡ��
						notEmpty_.wait(lock);//�൱�ڰ����������������notify_all �� notEmpty ʱ���ж�while(taskQue_.size() == 0) ���Ƿ����0������������whileѭ��
						//notEmpty_.wait(lock, [&]()->bool { return taskQue_.size() > 0; });
					}
				}
				idleThreadSize_--;
				//�̳߳�Ҫ������������Դ
				//if (!isPoolRunning_)
				//{
				//	break;//��ڶ���whileͬ���𣬵�����wait���������������Ѻ���תһȦ�󣬷���isPoolRunning_����Ϊfalse�����ٴ��жϣ�����
				//	//��ѭ����ֱ�Ӷ��߳��б��е��߳̽���ɾ�����൱���Ż����룬�������̳߳������������һ�����������
				//}
				std::cout << "tid:" << std::this_thread::get_id()
					<< "��ȡ����ɹ�" << std::endl;

				//��ʾ������в��գ���ʼȡ������
				// �����������ȡһ�������������ȡ�������񡱾���submitTask���Ǹ����񣡣���
				sp = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				//�������������֪ͨ�������е��߳�ִ�����񣬲������߳̿�������
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}
				//����������֪ͨ���Լ����������ύ����
				notFull_.notify_all();//notify_all֮��Ҫ�����ͷ�������Ȼ����notify_all�ˣ�submitTask�߳�Ҳ�޷�����
			}//�����������ͷŵ�������Ȼ���ŵ����Ļ����̳߳�ÿ��ֻ��һ���߳��������������������ͷţ���ɲ����̳߳ص�����

			//����߳���ִ���û��Զ��������ִ�е�ָ�
			if (nullptr != sp) {
				sp();
			}
			//����������
			idleThreadSize_++;
			auto lastTime = std::chrono::high_resolution_clock().now();//���µ�ǰ��ȷʱ��
		}
	}
	bool chekRunningState() const
	{
		return isPoolRunning_;
	}

private:
	//Thread��ͨ��new�����ģ�һ��new�Ļ�����Ҫdelete����ʹ��shared_ptr����ָ��Ͳ����ֶ�������
	//std::vector<Thread*> Threads_;//�߳��б�
	//std::vector<std::unique_ptr<Thread>> threads_;//�߳��б�threadpool.cpp�е�auto ptr ����threads_
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//����map��ӳ���߳�id���̶߳����ӳ��

	int initThreadSize_;//��ʼ�߳�����
	int threadSizeThreshHold_;//�߳�����������ֵ
	std::atomic_int curThreadSize_;//��¼��ǰ�̳߳������̵߳���������Ҳ����ʹ��map�е� .size����
	std::atomic_int idleThreadSize_;//��¼�̳߳��п����̵߳�����

	//Task���� -�� �������� ���õ����û���������Ϊ�ֲ����������ڵ�����
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;
	std::atomic_int taskSize_;//��������  ����̶߳�������������++--,���������̰߳�ȫ���⣬�����Ļ�����ɶ�������ģ�submitTask��ThreadFunc�߳�
	int taskQueMaxThreshHold_;//�����������������ֵ

	std::mutex taskQueMtx_; //��֤������е��̰߳�ȫ  Ӧ����submitTask��ThreadFunc��main���߳�(ThreadFunc����������)
	//����������������������в���ʱ���û��߳̿����������ύ�̣߳���������в���ʱ���̳߳��ڲ����߳̿���������
	//ȡ������������ѡ�
	std::condition_variable notFull_;//��ʾ������в���
	std::condition_variable notEmpty_;//��ʾ������в���
	std::condition_variable exitcond_;//��ʾ�ȴ��߳���Դȫ������

	PoolMode poolMode_; //��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_;//��ʾ��ǰ�̳߳ص�����״̬  

};
#endif