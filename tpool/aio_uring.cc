/* Copyright (C) 2021, MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#ifdef LINUX_URING
#include <liburing.h>
#endif

#include <algorithm>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>

#include "tpool.h"

namespace tpool
{

#ifdef LINUX_URING
class aio_uring final : public aio
{
public:
  aio_uring(int max_aio, thread_pool *tpool) : tpool_(tpool)
  {
    if (io_uring_queue_init(max_aio, &uring_, 0) != 0)
    {
      fprintf(stderr, "io_uring_queue_init() failed with errno %d\n", errno);
      abort();
    }
    thread_= std::thread{thread_routine, this};
  }

  ~aio_uring()
  {
    {
      std::lock_guard<std::mutex> _(mutex_);
      io_uring_sqe *sqe= io_uring_get_sqe(&uring_);
      io_uring_prep_nop(sqe);
      io_uring_sqe_set_data(sqe, nullptr);
      auto ret= io_uring_submit(&uring_);
      if (ret != 1)
      {
        fprintf(stderr, "io_uring_submit() returned %d during shutdown", ret);
        abort();
      }
    }
    thread_.join();
    io_uring_queue_exit(&uring_);
  }

  int submit_io(aiocb *cb) override
  {
    // The whole operation since io_uring_get_sqe() and till io_uring_submit()
    // must be atomical. This is because liburing provides thread-unsafe calls.
    std::lock_guard<std::mutex> _(mutex_);

    io_uring_sqe *sqe= io_uring_get_sqe(&uring_);
    if (cb->m_opcode == aio_opcode::AIO_PREAD)
      io_uring_prep_read(sqe, cb->m_fh, cb->m_buffer, cb->m_len, cb->m_offset);
    else
      io_uring_prep_write(sqe, cb->m_fh, cb->m_buffer, cb->m_len,
                          cb->m_offset);
    io_uring_sqe_set_data(sqe, cb);

    return io_uring_submit(&uring_) == 1 ? 0 : -1;
  }

  int bind(native_file_handle &fd) override
  {
    std::lock_guard<std::mutex> _(files_mutex_);
    auto it= std::lower_bound(files_.begin(), files_.end(), fd);
    assert(it == files_.end() || *it != fd);
    files_.insert(it, fd);
    return io_uring_register_files_update(&uring_, 0, files_.data(),
                                          files_.size());
  }

  int unbind(const native_file_handle &fd) override
  {
    std::lock_guard<std::mutex> _(files_mutex_);
    auto it= std::lower_bound(files_.begin(), files_.end(), fd);
    assert(*it == fd);
    files_.erase(it);
    return io_uring_register_files_update(&uring_, 0, files_.data(),
                                          files_.size());
  }

private:
  static void thread_routine(aio_uring *aio)
  {
    for (;;)
    {
      io_uring_cqe *cqe;
      if (int ret= io_uring_wait_cqe(&aio->uring_, &cqe))
      {
        fprintf(stderr, "io_uring_wait_cqe() returned %d\n", ret);
        abort();
      }

      aiocb *iocb= (aiocb *) io_uring_cqe_get_data(cqe);
      if (!iocb)
        break;

      int res= cqe->res;
      if (res < 0)
      {
        iocb->m_err= -res;
        iocb->m_ret_len= 0;
      }
      else
      {
        iocb->m_err= 0;
        iocb->m_ret_len= res;
      }

      io_uring_cqe_seen(&aio->uring_, cqe);

      iocb->m_internal_task.m_func= iocb->m_callback;
      iocb->m_internal_task.m_arg= iocb;
      iocb->m_internal_task.m_group= iocb->m_group;
      aio->tpool_->submit_task(&iocb->m_internal_task);
    }
  }

  io_uring uring_;
  std::mutex mutex_;
  thread_pool *tpool_;
  std::thread thread_;

  std::vector<native_file_handle> files_;
  std::mutex files_mutex_;
};

aio *create_uring_aio(thread_pool *pool, int max_aio)
{
  return new aio_uring(max_aio, pool);
}
#else
aio *create_uring_aio(thread_pool *, int) { return nullptr; }
#endif

} // namespace tpool
