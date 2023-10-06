#include "threadpool.h"
#include <future>

int sum1(int a, int b)
{
    return a + b;
}

int sum2(int a, int b, int c)
{
    return a + b + c;
}

int main()
{
   ThreadPool pool;

   pool.start();

   std::future<int> res1 = pool.submitTask(sum1, 10, 20);
   std::future<int> res2 = pool.submitTask(sum2, 1,2, 3);
   std::future<int> res3 = pool.submitTask([](int b, int e) -> int{
    int sum = 0;
    for(int i = b;i <= e;i++){
        sum += i;
    }
   }, 1, 100);

   std::cout << res1.get() << std::endl;
   std::cout << res2.get() << std::endl;
   std::cout << res3.get() << std::endl;

   getchar();
}