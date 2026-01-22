#pragma once
#include <unordered_map>
#include <iostream>
#include <vector>
#include <chrono>
#include <functional>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include<atomic>
#include<unordered_set>
class timer_scheduler 
{
private:
    struct timer_info 
    {
        size_t id=0;
        std::chrono::steady_clock::time_point end;
        std::chrono::milliseconds interval;
        std::function<void()> callback;
        bool repeat;
    };
    
    std::vector<timer_info> timers;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool>running{false};
    size_t next_id = 1;//添加一个timer就加1
    std::thread loop_threads;
    std::vector<timer_info> on_time_timers;
    std::vector<size_t> on_time_ids;
    std::unordered_set<size_t>canceled_ids;
    void event_loop() 
    {
        while(running) 
        {
            auto now = std::chrono::steady_clock::now();
            on_time_ids.clear();
            this->on_time_timers.clear();

            {
                std::lock_guard<std::mutex> lock(mutex);
                
                // 找到所有到点的定时器
                for(auto it = timers.begin(); it != timers.end();) 
                {
                    if(now >= it->end) 
                    {
                        on_time_timers.push_back(*it);
                        on_time_ids.push_back(it->id);
                        it = timers.erase(it);//到点的定时器就从timers移除
                    } 
                    else 
                    {
                        ++it;
                    }
                }
            }


            for(auto& timer : on_time_timers) 
            {
                    bool is_cancel=false;

                    //检查是否被取消
                    {
                        std::lock_guard<std::mutex>lock(this->mutex);
                        if (this->canceled_ids.find(timer.id)!=this->canceled_ids.end())
                        {
                            is_cancel=true;
                            this->canceled_ids.erase(timer.id);
                        }
                        
                    }
                    
                    if (is_cancel)
                    {
                        continue;
                    }
                    
                    timer.callback();

                // 重新添加重复定时器  如果是周期性任务，并且没有被取消，那么就刷新到点时间，
                    if(timer.repeat) 
                    {
                        timer.end = now + timer.interval;
                        add_timer(timer);
                    }
            }
            
            // 计算到下一个定时器的时间
            std::chrono::milliseconds sleep_time(10); // 默认休眠10ms


            //计算休眠时间
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(!timers.empty()) 
                {
                    auto next_end = std::min_element(timers.begin(), timers.end(),
                        [](const timer_info& a, const timer_info& b) {
                            return a.end < b.end;
                        })->end;
                    
                    auto time_until_next = std::chrono::duration_cast<std::chrono::milliseconds>
                    (next_end - std::chrono::steady_clock::now());

                    if(time_until_next.count() > 0) 
                    {
                        sleep_time = time_until_next;
                    }
                    else 
                    {
                        sleep_time = std::chrono::milliseconds(1); // 至少休眠1ms
                    }
                }
            }

            // 休眠或等待唤醒
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, sleep_time);
        }
    }

    size_t add_timer(timer_info timer) 
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (timer.id==0)
        {
            timer.id=this->next_id;
            this->next_id++;
        }
        
        timers.push_back(timer);
        cv.notify_one();
        return timer.id;
    }


    
public:
    void start()
    {
        std::lock_guard<std::mutex>lock(this->mutex);
        this->running=true;

        this->loop_threads=std::thread(&timer_scheduler::event_loop,this);
    }

    void stop()
    {
        std::lock_guard<std::mutex>lock(this->mutex);
        this->running=false;
    }

    timer_scheduler() =default;

    
    ~timer_scheduler() 
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
        }
        cv.notify_one();
        if(loop_threads.joinable()) loop_threads.join();
    }
    
    size_t schedule_after(int milliseconds,std::function<void()> callback ) {
        timer_info timer;
        timer.end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        timer.interval = std::chrono::milliseconds(milliseconds);
        timer.callback = callback;
        timer.repeat = false;
        return add_timer(timer);
    }
    

    size_t schedule_every(int milliseconds,std::function<void()> callback ) {
        timer_info timer;
        timer.end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        timer.interval = std::chrono::milliseconds(milliseconds);
        timer.callback = callback;
        timer.repeat = true;
        return add_timer(timer);
    }

    
    //在哪个数组里删除
    //timers
    //on_time_timers    如果是删除这个，那么在执行回调函数时取消会出问题，因为都是涉及on_time_timers
    bool cancel(size_t id) 
    {
        std::lock_guard<std::mutex> lock(mutex);

        auto erase_pos=std::find_if(this->timers.begin(),this->timers.end(),[&id](const timer_info& timer){ return timer.id==id;});

        if (erase_pos!=this->timers.end())
        {
            this->timers.erase(erase_pos);
        }
        
        auto is_find=std::find_if(this->timers.begin(),this->timers.end(),[&id](const timer_info& timer){ return timer.id==id;});
        
        this->canceled_ids.emplace(id);
        this->cv.notify_one();

        return is_find==this->timers.end();
    }
};
