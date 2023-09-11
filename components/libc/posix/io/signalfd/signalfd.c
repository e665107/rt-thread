/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-08-29   zmq810150896   first version
 */

#include <rtthread.h>
#include <sys/signalfd.h>
#include <dfs_file.h>
#include <signal.h>
#include <rthw.h>
#include <sys/time.h>
#include <lwp_signal.h>
#include <lwp.h>
#include <poll.h>

#define SIGNALFD_MUTEX_NAME "signalfd"
#define SIGNALFD_SHART_MAX 10

static int is_head_init = 0;

struct rt_signalfd_ctx
{
    sigset_t sigmask;
    struct rt_mutex lock;
    siginfo_t info;
    int tick;
    rt_wqueue_t signalfd_queue;
    struct rt_lwp *lwp[SIGNALFD_SHART_MAX];
};

static int signalfd_close(struct dfs_file *file);
static int signalfd_poll(struct dfs_file *file, struct rt_pollreq *req);
#ifndef RT_USING_DFS_V2
static ssize_t signalfd_read(struct dfs_file *file, void *buf, size_t count);
#else
static ssize_t signalfd_read(struct dfs_file *file, void *buf, size_t count, off_t *pos);
#endif
static int signalfd_add_notify(struct rt_signalfd_ctx *sfd);

static const struct dfs_file_ops signalfd_fops =
{
    .close      = signalfd_close,
    .poll       = signalfd_poll,
    .read       = signalfd_read,
};

static int signalfd_close(struct dfs_file *file)
{
    struct rt_signalfd_ctx *sfd;

    if (file->vnode->ref_count != 1)
        return 0;

    sfd = file->vnode->data;

    if (sfd)
    {
        rt_mutex_detach(&sfd->lock);
        rt_free(sfd);
    }

    return 0;
}

static int signalfd_poll(struct dfs_file *file, struct rt_pollreq *req)
{
    struct rt_signalfd_ctx *sfd;
    int events = 0;

    if (file->vnode)
    {
        sfd = file->vnode->data;

        rt_poll_add(&sfd->signalfd_queue, req);
        signalfd_add_notify(sfd);

        rt_mutex_take(&sfd->lock, RT_WAITING_FOREVER);

        if (sfd->tick)
            events |= POLLIN;

        sfd->tick = 0;
        rt_mutex_release(&sfd->lock);
    }

    return events;
}

#ifndef RT_USING_DFS_V2
static ssize_t signalfd_read(struct dfs_file *file, void *buf, size_t count)
#else
static ssize_t signalfd_read(struct dfs_file *file, void *buf, size_t count, off_t *pos)
#endif
{
    struct rt_signalfd_ctx *sfd;
    struct signalfd_siginfo *buffer;
    rt_err_t ret = -1;

    if (sizeof(struct signalfd_siginfo) > count)
        return -1;

    buffer = (struct signalfd_siginfo *)buf;

    if (file->vnode)
    {
        sfd = file->vnode->data;

        signalfd_add_notify(sfd);

        rt_wqueue_wait(&sfd->signalfd_queue, 0, RT_WAITING_FOREVER);
        rt_mutex_take(&sfd->lock, RT_WAITING_FOREVER);
        sfd->tick = 1;
        rt_mutex_release(&sfd->lock);

        buffer->ssi_signo = sfd->info.si_signo;
        buffer->ssi_code = sfd->info.si_code;

        ret = sizeof(struct signalfd_siginfo);
    }

    return ret;
}

static void signalfd_callback(rt_wqueue_t *signalfd_queue, int signum)
{
    struct rt_signalfd_ctx *sfd;

    sfd = rt_container_of(signalfd_queue, struct rt_signalfd_ctx, signalfd_queue);

    if (sfd)
    {
        for (int sig = 1; sig < NSIG; sig++)
        {
            if (sigismember(&sfd->sigmask, signum))
            {
                rt_mutex_take(&sfd->lock, RT_WAITING_FOREVER);
                sfd->tick = 1;
                sfd->info.si_signo = signum;
                rt_mutex_release(&sfd->lock);
                rt_wqueue_wakeup(signalfd_queue, (void*)POLLIN);
            }
        }
    }
}

static int signalfd_add_notify(struct rt_signalfd_ctx *sfd)
{
    struct rt_lwp_notify *lwp_notify;
    rt_err_t ret = -1;
    rt_slist_t *node;
    int is_lwp = 0;

    rt_mutex_take(&sfd->lock, RT_WAITING_FOREVER);

    for (int i = 0; i < is_head_init; i++)
    {
        if (sfd->lwp[i])
        {
            if (sfd->lwp[i] == lwp_self())
            {
                is_lwp = 1;
            }
        }
    }

    if (is_lwp == 0)
    {
        sfd->lwp[is_head_init] = lwp_self();

        if (is_head_init == 0)
        {
            rt_slist_init(&sfd->lwp[is_head_init]->signalfd_notify_head);
        }

        lwp_notify = (struct rt_lwp_notify *)rt_malloc(sizeof(struct rt_lwp_notify));
        if (lwp_notify)
        {
            lwp_notify->notify = signalfd_callback;
            lwp_notify->signalfd_queue = &sfd->signalfd_queue;
            rt_slist_append(&sfd->lwp[is_head_init]->signalfd_notify_head, &(lwp_notify->list_node));

            is_head_init ++;
            ret = 0;
        }
        else
        {
            rt_slist_for_each(node, &sfd->lwp[is_head_init]->signalfd_notify_head)
            {
                struct rt_lwp_notify *n = rt_slist_entry(node, struct rt_lwp_notify, list_node);
                rt_slist_remove(&sfd->lwp[is_head_init]->signalfd_notify_head, &n->list_node);
                rt_free(n);
            }
            rt_set_errno(ENOMEM);
        }
    }

    rt_mutex_release(&sfd->lock);

    return ret;
}

static int signalfd_do(int fd, const sigset_t *mask, int flags)
{
    struct dfs_file *df;
    struct rt_signalfd_ctx *sfd;
    rt_err_t ret = 0;

    if (fd == -1)
    {
        fd = fd_new();
        if (fd < 0)
            return -1;

        ret = fd;
        df = fd_get(fd);

        if (df)
        {
            sfd = (struct rt_signalfd_ctx *)rt_malloc(sizeof(struct rt_signalfd_ctx));
            if (sfd)
            {
                df->vnode = (struct dfs_vnode *)rt_malloc(sizeof(struct dfs_vnode));
                if (df->vnode)
                {
                    dfs_vnode_init(df->vnode, FT_REGULAR, &signalfd_fops);
                    df->vnode->data = sfd;

                    for (int i = 0; i < is_head_init; i++)
                    {
                        sfd->lwp[i] = RT_NULL;
                    }

                    sigemptyset(&sfd->sigmask);
                    memcpy(&sfd->sigmask, mask, sizeof(sigset_t));

                    rt_mutex_init(&sfd->lock, SIGNALFD_MUTEX_NAME, RT_IPC_FLAG_FIFO);
                    rt_wqueue_init(&sfd->signalfd_queue);

                    if (signalfd_add_notify(sfd) < 0)
                    {
                        is_head_init = 0;
                        fd_release(fd);
                        rt_free(sfd);
                        ret = -1;
                    }

                    sfd->tick = 0;

                    df->flags |= flags;

                    #ifdef RT_USING_DFS_V2
                    df->fops = &signalfd_fops;
                    #endif
                }
                else
                {
                    fd_release(fd);
                    rt_free(sfd);
                    ret = -1;
                }
            }
            else
            {
                fd_release(fd);
                ret = -1;
            }
        }
        else
        {
            fd_release(fd);
        }
    }
    else
    {
        df = fd_get(fd);
        if (df)
        {
            sfd = df->vnode->data;
            df->flags = flags;
            sigemptyset(&sfd->sigmask);
            memcpy(&sfd->sigmask, mask, sizeof(sigset_t));
            ret = fd;
        }
        else
        {
            rt_set_errno(EBADF);
            ret = -1;
        }
    }

    return ret;
}

int signalfd(int fd, const sigset_t *mask, int flags)
{
    return signalfd_do(fd, mask, flags);
}
