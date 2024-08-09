# ThreadPool
基于c++11的可变参模板的异步线程池，可支持两种模式用于用户定制，提供future接口用于返回值
//给线程池提交任务
//Result submitTask(std::shared_ptr<Task> sp);
// 给线程池提交任务
// 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
// pool.submitTask(sum1, 10, 20);   csdn  大秦坑王  右值引用+引用折叠原理
// 返回值future<>
