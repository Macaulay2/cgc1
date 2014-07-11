#include "internal_declarations.hpp"
#include <cgc1/declarations.hpp>
#include <algorithm>
#include <signal.h>
#include <cgc1/posix.hpp>
#include <cgc1/cgc1.hpp>
#include <cgc1/concurrency.hpp>
#include "global_kernel_state.hpp"
#include "thread_local_kernel_state.hpp"
#include "allocator.hpp"
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace cgc1
{
  namespace details
  {
    void *internal_allocate(size_t n)
    {
      return g_gks._internal_allocator().initialize_thread().allocate(n);
    }
    void internal_deallocate(void *p)
    {
      g_gks._internal_allocator().initialize_thread().destroy(p);
    }
    void *internal_slab_allocate(size_t n)
    {
      return g_gks._internal_slab_allocator().allocate_raw(n);
    }
    void internal_slab_deallocate(void *p)
    {
      g_gks._internal_slab_allocator().deallocate_raw(p);
    }
    global_kernel_state_t::global_kernel_state_t() : m_slab_allocator(50000000, 50000000), m_num_collections(0)
    {
      m_enabled_count = 1;
      details::initialize_tlks();
    }
    void global_kernel_state_t::shutdown()
    {
      CGC1_CONCURRENCY_LOCK_GUARD(m_mutex);
      m_gc_threads.clear();
    }
    void global_kernel_state_t::enable()
    {
      m_enabled_count++;
    }
    void global_kernel_state_t::disable()
    {
      m_enabled_count--;
    }
    bool global_kernel_state_t::enabled() const
    {
      return m_enabled_count > 0;
    }
    object_state_t* global_kernel_state_t::_u_find_valid_object_state(void* addr) const
    {
      auto handle = details::g_gks.gc_allocator()._u_find_block(addr);
      if (!handle)
        return nullptr;
      object_state_t* os = handle->m_block->find_address(addr);
      if (!os || !os->in_use())
        return nullptr;
      return os;
    }
    object_state_t* global_kernel_state_t::find_valid_object_state(void* addr) const
    {
      CGC1_CONCURRENCY_LOCK_GUARD(m_mutex);
      return _u_find_valid_object_state(addr);
    }
    void global_kernel_state_t::add_root(void **r)
    {
      wait_for_collection();
      ::std::unique_lock<decltype(m_mutex)> l(m_mutex, ::std::adopt_lock);
      auto it = find(m_roots.begin(), m_roots.end(), r);
      if (it == m_roots.end())
        m_roots.push_back(r);
    }
    void global_kernel_state_t::remove_root(void **r)
    {
      wait_for_collection();
      ::std::unique_lock<decltype(m_mutex)> l(m_mutex, ::std::adopt_lock);
      auto it = find(m_roots.begin(), m_roots.end(), r);
      if (it != m_roots.end())
        m_roots.erase(it);
    }
    size_t global_kernel_state_t::num_collections() const
    {
      return m_num_collections;
    }
    void global_kernel_state_t::_u_setup_gc_threads()
    {
      if (m_gc_threads.empty())
        return;
      size_t cur_gc_thread = 0;
      for (auto &thread : m_gc_threads) {
        thread->reset();
      }
      for (auto state : m_threads) {
        m_gc_threads[cur_gc_thread]->add_thread(state->thread_id());
        cur_gc_thread = (cur_gc_thread + 1) % m_gc_threads.size();
      }
      auto &blocks = m_gc_allocator._u_blocks();
      size_t num_gc_threads = m_gc_threads.size();
      size_t num_blocks = blocks.size();
      size_t block_per_thread = num_blocks / num_gc_threads;
      size_t num_roots = m_roots.size();
      size_t roots_per_thread = num_roots / num_gc_threads;
      for (size_t i = 0; i < m_gc_threads.size() - 1; ++i) {
        m_gc_threads[i]->set_allocator_blocks(blocks.data() + block_per_thread * i, blocks.data() + block_per_thread * (i + 1));
        m_gc_threads[i]->set_root_iterators(m_roots.data() + roots_per_thread * i, m_roots.data() + roots_per_thread * (i + 1));
      }
      m_gc_threads.back()->set_allocator_blocks(blocks.data() + block_per_thread * (num_gc_threads - 1),
                                                blocks.data() + num_blocks);
      m_gc_threads.back()->set_root_iterators(m_roots.data() + roots_per_thread * (num_gc_threads - 1),
                                              m_roots.data() + num_roots);
    }
    void global_kernel_state_t::wait_for_finalization()
    {
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->wait_until_finalization_finished();
      }
    }
    template <typename T1, typename T2, typename T3, typename T4>
    void lock(T1 &t1, T2 &t2, T3 &t3, T4 &t4)
        _Exclusive_Lock_Function_(t1, t2.m_mutex, t3.m_mutex, t4) _No_Thread_Safety_Analysis_
    {
      ::std::lock(t1, t2, t3, t4);
    }
    void global_kernel_state_t::collect()
    {
      {
        CGC1_CONCURRENCY_LOCK_GUARD(m_thread_mutex);
        for (auto &gc_thread : m_gc_threads) {
          if (!gc_thread->finalization_finished())
            return;
        }
      }
      force_collect();
    }
    void global_kernel_state_t::force_collect()
    {
      wait_for_finalization();
      // grab allocator locks so that they are in a consistent state for garbage collection.
      lock(m_mutex, m_gc_allocator, m_cgc_allocator, m_slab_allocator, m_thread_mutex);
      // make sure we aren't already collecting
      if (m_collect || m_num_paused_threads != m_num_resumed_threads) {
        m_thread_mutex.unlock();
        m_gc_allocator.unlock();
        m_cgc_allocator.unlock();
        m_slab_allocator.unlock();
        m_mutex.unlock();
        return;
      }
      m_collect = true;
      m_freed_in_last_collection.clear();
      m_num_paused_threads = m_num_resumed_threads = 0;
      _u_suspend_threads();
      m_thread_mutex.unlock();
      get_tlks()->set_stack_ptr(cgc1_builtin_current_stack());
      // release allocator locks so they can be used.
      m_gc_allocator.unlock();
      m_cgc_allocator.unlock();
      m_slab_allocator.unlock();
      if (m_gc_allocator._u_blocks().empty()) {
        m_num_collections++;
        m_collect = false;
        CGC1_CONCURRENCY_LOCK_GUARD(m_thread_mutex);
        _u_resume_threads();
        m_mutex.unlock();
        return;
      }
      // do collection
      _u_setup_gc_threads();
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->start_clear();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->wait_until_clear_finished();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->start_mark();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->wait_until_mark_finished();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->start_sweep();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->wait_until_sweep_finished();
      }
      for (auto &gc_thread : m_gc_threads) {
        gc_thread->notify_all_threads_resumed();
      }
      m_num_collections++;
      m_collect = false;
      m_thread_mutex.lock();
      _u_resume_threads();
      m_thread_mutex.unlock();
      m_mutex.unlock();
    }
    cgc_internal_vector_t<void *> global_kernel_state_t::_d_freed_in_last_collection() const
    {
      CGC1_CONCURRENCY_LOCK_GUARD(m_mutex);
      return m_freed_in_last_collection;
    }
    void global_kernel_state_t::wait_for_collection()
    {
      ::std::unique_lock<decltype(m_mutex)> lock(m_mutex);
      while (m_collect) {
#ifndef _WIN32
        m_start_world_condition.wait(lock);
#else
        lock.unlock();
        ::std::this_thread::yield();
        lock.lock();
#endif
      }
      lock.release();
    }
    void global_kernel_state_t::wait_for_collection2()
    {
      double_lock_t<decltype(m_mutex), decltype(m_thread_mutex)> lock(m_mutex, m_thread_mutex);
      while (m_collect) {
#ifndef _WIN32
        m_start_world_condition.wait(lock);
#else
        lock.unlock();
        ::std::this_thread::yield();
        lock.lock();
#endif
      }
      lock.release();
    }
    void global_kernel_state_t::initialize_current_thread(void *top_of_stack)
    {
      auto tlks = details::get_tlks();
      // do not start creating threads during an ongoing collection.
      wait_for_collection2();
      ::std::unique_lock<decltype(m_mutex)> lock(m_mutex, ::std::adopt_lock);
      ::std::unique_lock<decltype(m_thread_mutex)> lock2(m_thread_mutex, ::std::adopt_lock);
      if (tlks == nullptr) {
        tlks = make_unique_allocator<details::thread_local_kernel_state_t, cgc_internal_malloc_allocator_t<void>>().release();
        set_tlks(tlks);
        _u_initialize();
      } else
        abort();
      tlks->set_top_of_stack(top_of_stack);
      m_threads.push_back(tlks);
      m_cgc_allocator.initialize_thread();
      m_gc_allocator.initialize_thread();
    }
    void global_kernel_state_t::destroy_current_thread()
    {
      auto tlks = details::get_tlks();
      // do not start destroying threads during an ongoing collection.
      wait_for_collection2();
      ::std::unique_lock<decltype(m_mutex)> lock(m_mutex, ::std::adopt_lock);
      ::std::unique_lock<decltype(m_thread_mutex)> lock2(m_thread_mutex, ::std::adopt_lock);
      // find thread and erase it from global state
      auto it = find(m_threads.begin(), m_threads.end(), tlks);
      if (it == m_threads.end()) {
        assert(0);
        return;
      }
      m_gc_allocator.destroy_thread();
      m_cgc_allocator.destroy_thread();
      m_threads.erase(it);
      ::std::unique_ptr<details::thread_local_kernel_state_t, cgc_internal_malloc_deleter_t> tlks_deleter(tlks);
    }
    void global_kernel_state_t::_u_initialize()
    {
      if (m_initialized)
        return;
#ifndef _WIN32
      details::initialize_thread_suspension();
#endif
      m_gc_allocator.initialize(pow2(29), pow2(36));
      if (!::std::thread::hardware_concurrency()) {
        ::std::cerr << "std::thread::hardware_concurrency not well defined\n";
        abort();
      }
      // for (size_t i = 0; i < ::std::thread::hardware_concurrency(); ++i)
      m_gc_threads.emplace_back(new gc_thread_t());
      m_initialized = true;
    }
#ifndef _WIN32
    void global_kernel_state_t::_u_suspend_threads()
    {
      ::std::unique_lock<decltype(m_mutex)> lock(m_mutex, ::std::adopt_lock);
      for (auto state : m_threads) {
        if (state->thread_id() == ::std::this_thread::get_id())
          continue;
        cgc1::pthread_kill(state->thread_handle(), SIGUSR1);
      }
      // wait for all threads to stop
      while (m_num_paused_threads != m_threads.size() - 1)
        m_stop_world_condition.wait(lock);
      lock.release();
    }
    void global_kernel_state_t::_u_resume_threads()
    {
      m_start_world_condition.notify_all();
    }
    void global_kernel_state_t::_collect_current_thread()
    {
      auto tlks = details::get_tlks();
      if (!tlks)
        return;
      unique_lock_t<decltype(m_mutex)> lock(m_mutex);
      tlks->set_stack_ptr(__builtin_frame_address(0));
      m_num_paused_threads++;
      m_stop_world_condition.notify_all();
      while (m_collect) {
        m_start_world_condition.wait(lock);
      }
      m_num_resumed_threads++;
      lock.unlock();
      tlks->set_in_signal_handler(false);
    }
#else
    void global_kernel_state_t::_u_suspend_threads()
    {
      for (auto it = m_threads.begin() + 1; it != m_threads.end(); ++it) {
        auto state = *it;
        for (size_t i = 0; i < 5000; ++i) {
          // bug in Windows function spec!
          int ret = static_cast<int>(::SuspendThread(state->thread_handle()));
          if (ret == 0) {
            state->set_in_signal_handler(true);
            break;
          } else if (ret < 0) {
            m_mutex.unlock();
            m_gc_allocator.unlock();
            m_cgc_allocator.unlock();
            ::std::lock(m_mutex, m_gc_allocator, m_cgc_allocator);
          } else {
            abort();
          }
        }
        if (!state->in_signal_handler()) {
          ::abort();
        }
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_FULL;
        auto ret = ::GetThreadContext(state->thread_handle(), &context);
        if (!ret) {
          ::abort();
        }
        uint8_t *stack_ptr = nullptr;
#ifdef _WIN64
        stack_ptr = reinterpret_cast<uint8_t *>(context.Rsp);
#else
        stack_ptr = reinterpret_cast<uint8_t *>(context.Esp);
#endif
        if (!stack_ptr)
          abort();
        state->set_stack_ptr(stack_ptr);
      }
    }
    void global_kernel_state_t::_u_resume_threads()
    {
      for (auto state : m_threads) {
        if (state->thread_id() == ::std::this_thread::get_id())
          continue;
        state->set_stack_ptr(nullptr);
        if (static_cast<int>(::ResumeThread(state->thread_handle())) < 0)
          abort();
        state->set_in_signal_handler(false);
      }
    }
    void global_kernel_state_t::_collect_current_thread()
    {
    }
#endif
  }
}