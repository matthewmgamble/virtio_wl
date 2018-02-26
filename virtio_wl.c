/*
 *  Wayland Virtio Driver
 *  Copyright (C) 2017 Google, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

/*
Virtio Wayland (virtio_wl or virtwl) is a virtual device that allows a guest
virtual machine to use a wayland server on the host transparently (to the host).
This is done by proxying the wayland protocol socket stream verbatim between the
host and guest over 2 (recv and send) virtio queues. The guest can request new
wayland server connections to give each guest wayland client a different server
context. Each host connection's file descriptor is exposed to the guest as a
virtual file descriptor (VFD). Additionally, the guest can request shared memory
file descriptors which are also exposed as VFDs. These shared memory VFDs are
directly writable by the guest via device memory injected by the host. Each VFD
is sendable along a connection context VFD and will appear as ancillary data to
the wayland server, just like a message from an ordinary wayland client. When
the wayland server sends a shared memory file descriptor to the client (such as
when sending a keymap), a VFD is allocated by the device automatically and its
memory is injected into as device memory.

This driver is intended to be paired with the `virtwl_guest_proxy` program which
is run in the guest system and acts like a wayland server. It accepts wayland
client connections and converts their socket messages to ioctl messages exposed
by this driver via the `/dev/wl` device file. While it would be possible to
expose a unix stream socket from this driver, the user space helper is much
cleaner to write.
*/

#include <linux/anon_inodes.h>
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/scatterlist.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include "virtio_wl.h"

#define VFD_ILLEGAL_SIGN_BIT 0x80000000
#define VFD_HOST_VFD_ID_BIT 0x40000000

struct virtwl_vfd_qentry {
	struct list_head list;
	struct virtio_wl_ctrl_hdr *hdr;
	unsigned int len; /* total byte length of ctrl_vfd_* + vfds + data */
	unsigned int vfd_offset; /* int offset into vfds */
	unsigned int data_offset; /* byte offset into data */
};

struct virtwl_vfd {
	struct kobject kobj;
	struct mutex lock;

	struct virtwl_info *vi;
	uint32_t id;
	uint32_t flags;
	uint64_t pfn;
	uint32_t size;

	struct list_head in_queue; /* list of virtwl_vfd_qentry */
	wait_queue_head_t in_waitq;
};

struct virtwl_info {
	dev_t dev_num;
	struct device *dev;
	struct class *class;
	struct cdev cdev;

	struct mutex vq_locks[VIRTWL_QUEUE_COUNT];
	struct virtqueue *vqs[VIRTWL_QUEUE_COUNT];
	struct work_struct in_vq_work;
	struct work_struct out_vq_work;

	wait_queue_head_t out_waitq;

	struct mutex vfds_lock;
	struct idr vfds;
};

static struct virtwl_vfd *virtwl_vfd_alloc(struct virtwl_info *vi);
static void virtwl_vfd_free(struct virtwl_vfd *vfd);

static struct file_operations virtwl_vfd_fops;

static int virtwl_resp_err(unsigned int type)
{
	switch (type) {
		case VIRTIO_WL_RESP_OK:
		case VIRTIO_WL_RESP_VFD_NEW:
			return 0;
		case VIRTIO_WL_RESP_ERR:
			return -ENODEV; /* Device is no longer reliable */
		case VIRTIO_WL_RESP_OUT_OF_MEMORY:
			return -ENOMEM;
		case VIRTIO_WL_RESP_INVALID_ID:
		case VIRTIO_WL_RESP_INVALID_TYPE:
		default:
			return -EINVAL;
	}
}

static int vq_return_inbuf_locked(struct virtqueue *vq, void *buffer)
{
	int ret;
	struct scatterlist sg[1];
	sg_init_one(sg, buffer, PAGE_SIZE);

	ret = virtqueue_add_inbuf(vq, sg, 1, buffer, GFP_KERNEL);
	if (ret) {
		printk("virtwl: failed to give inbuf to host: %d\n", ret);
		return ret;
	}

	return 0;
}

static int vq_queue_out(struct virtwl_info *vi, struct scatterlist *out_sg,
			struct scatterlist *in_sg,
			struct completion *finish_completion,
			bool nonblock)
{
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_OUT];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_OUT];
	struct scatterlist *sgs[] = { out_sg, in_sg };
	int ret = 0;

	mutex_lock(vq_lock);
	while ((ret = virtqueue_add_sgs(vq, sgs, 1, 1, finish_completion,
				        GFP_KERNEL)) == -ENOSPC) {
		mutex_unlock(vq_lock);
		if (nonblock)
			return -EAGAIN;
		if (!wait_event_timeout(vi->out_waitq, vq->num_free > 0, HZ))
			return -EBUSY;
		mutex_lock(vq_lock);
	}
	if (!ret)
		virtqueue_kick(vq);
	mutex_unlock(vq_lock);

	return ret;
}

static int vq_fill_locked(struct virtqueue *vq)
{
	void *buffer;
	int ret = 0;

	while (vq->num_free > 0) {
		buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!buffer) {
			ret = -ENOMEM;
			goto clear_queue;
		}

		ret = vq_return_inbuf_locked(vq, buffer);
		if (ret)
			goto clear_queue;
	}

	return 0;

clear_queue:
	while ((buffer = virtqueue_detach_unused_buf(vq))) {
		kfree(buffer);
	}
	return ret;
}

static bool vq_handle_new(struct virtwl_info *vi,
			  struct virtio_wl_ctrl_vfd_new *new, unsigned int len)
{
	struct virtwl_vfd *vfd;
	u32 id = new->vfd_id;
	int ret;

	if (id == 0)
		return true; /* return the inbuf to vq */

	if (!(id & VFD_HOST_VFD_ID_BIT) || (id & VFD_ILLEGAL_SIGN_BIT)) {
		printk("virtwl: received a vfd with invalid id: %u\n", id);
		return true; /* return the inbuf to vq */
	}

	vfd = virtwl_vfd_alloc(vi);
	if (!vfd)
		return true; /* return the inbuf to vq */

	mutex_lock(&vi->vfds_lock);
	ret = idr_alloc(&vi->vfds, vfd, id, id + 1, GFP_KERNEL);
	mutex_unlock(&vi->vfds_lock);

	if (ret <= 0) {
		virtwl_vfd_free(vfd);
		printk("virtwl: failed to place received vfd: %d\n", ret);
		return true; /* return the inbuf to vq */
	}

	vfd->id = id;
	vfd->size = new->size;
	vfd->pfn = new->pfn;
	vfd->flags = new->flags;

	return true; /* return the inbuf to vq */
}

static bool vq_handle_recv(struct virtwl_info *vi,
			   struct virtio_wl_ctrl_vfd_recv *recv,
			   unsigned int len)
{
	struct virtwl_vfd *vfd;
	struct virtwl_vfd_qentry *qentry;

	mutex_lock(&vi->vfds_lock);
	vfd = idr_find(&vi->vfds, recv->vfd_id);
	if (vfd)
		mutex_lock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);

	if (!vfd) {
		printk("virtwl: recv for unknown vfd_id %u\n", recv->vfd_id);
		return true; /* return the inbuf to vq */
	}

	qentry = kzalloc(sizeof(*qentry), GFP_KERNEL);
	if (!qentry) {
		mutex_unlock(&vfd->lock);
		printk("virtwl: failed to allocate qentry for vfd\n");
		return true; /* return the inbuf to vq */
	}

	qentry->hdr = &recv->hdr;
	qentry->len = len;

	list_add_tail(&qentry->list, &vfd->in_queue);
	wake_up_interruptible(&vfd->in_waitq);
	mutex_unlock(&vfd->lock);

	return false; /* no return the inbuf to vq */
}

static bool vq_dispatch_hdr(struct virtwl_info *vi, unsigned int len,
			    struct virtio_wl_ctrl_hdr *hdr)
{
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	bool return_vq = true;
	int ret;

	switch (hdr->type) {
	case VIRTIO_WL_CMD_VFD_NEW:
		return_vq = vq_handle_new(vi,
					  (struct virtio_wl_ctrl_vfd_new *)hdr,
					  len);
		break;
	case VIRTIO_WL_CMD_VFD_RECV:
		return_vq = vq_handle_recv(vi,
			(struct virtio_wl_ctrl_vfd_recv *)hdr, len);
		break;
	default:
		printk("virtwl: unhandled ctrl command: %u\n", hdr->type);
		break;
	}

	if (!return_vq)
		return false; /* no kick the vq */

	mutex_lock(vq_lock);
	ret = vq_return_inbuf_locked(vq, hdr);
	mutex_unlock(vq_lock);
	if (ret) {
		printk("virtwl: failed to return inbuf to host: %d\n", ret);
		kfree(hdr);
	}

	return true; /* kick the vq */
}

static void vq_in_work_handler(struct work_struct *work)
{
	struct virtwl_info *vi = container_of(work, struct virtwl_info,
					      in_vq_work);
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	void *buffer;
	unsigned int len;
	bool kick_vq = false;

	mutex_lock(vq_lock);
	while ((buffer = virtqueue_get_buf(vq, &len)) != NULL) {
		struct virtio_wl_ctrl_hdr *hdr = buffer;
		mutex_unlock(vq_lock);
		kick_vq |= vq_dispatch_hdr(vi, len, hdr);
		mutex_lock(vq_lock);
	}
	mutex_unlock(vq_lock);

	if (kick_vq)
		virtqueue_kick(vq);
}

static void vq_out_work_handler(struct work_struct *work)
{
	struct virtwl_info *vi = container_of(work, struct virtwl_info,
					      out_vq_work);
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_OUT];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_OUT];
	unsigned int len;
	struct completion *finish_completion;
	bool wake_waitq = false;

	mutex_lock(vq_lock);
	while ((finish_completion = virtqueue_get_buf(vq, &len)) != NULL) {
		wake_waitq = true;
		complete(finish_completion);
	}
	mutex_unlock(vq_lock);

	if (wake_waitq)
		wake_up_interruptible(&vi->out_waitq);
}

static void vq_in_cb(struct virtqueue *vq)
{
	struct virtwl_info *vi = vq->vdev->priv;
	schedule_work(&vi->in_vq_work);
}

static void vq_out_cb(struct virtqueue *vq)
{
	struct virtwl_info *vi = vq->vdev->priv;
	schedule_work(&vi->out_vq_work);
}

static struct virtwl_vfd *virtwl_vfd_alloc(struct virtwl_info *vi)
{
	struct virtwl_vfd *vfd = kzalloc(sizeof(struct virtwl_vfd), GFP_KERNEL);
	if (!vfd)
		return ERR_PTR(-ENOMEM);

	vfd->vi = vi;

	mutex_init(&vfd->lock);
	INIT_LIST_HEAD(&vfd->in_queue);
	init_waitqueue_head(&vfd->in_waitq);

	return vfd;
}

/* Locks the vfd and unlinks its id from vi */
static void virtwl_vfd_lock_unlink(struct virtwl_vfd *vfd)
{
	struct virtwl_info *vi = vfd->vi;
	/* this order is important to avoid deadlock */
	mutex_lock(&vi->vfds_lock);
	mutex_lock(&vfd->lock);
	idr_remove(&vi->vfds, vfd->id);
	mutex_unlock(&vi->vfds_lock);
}

/*
 * Only used to free a vfd that is not referenced any place else and contains
 * no queed virtio buffers. This must not be called while vfd is included in a
 * vi->vfd.
 */
static void virtwl_vfd_free(struct virtwl_vfd *vfd)
{
	kfree(vfd);
}

/*
 * Thread safe and also removes vfd from vi as well as any queued virtio buffers
 */
static void virtwl_vfd_remove(struct virtwl_vfd *vfd)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	struct virtwl_vfd_qentry *qentry, *next;
	virtwl_vfd_lock_unlink(vfd);

	mutex_lock(vq_lock);
	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		vq_return_inbuf_locked(vq, qentry->hdr);
		list_del(&qentry->list);
		kfree(qentry);
	}
	mutex_unlock(vq_lock);

	virtwl_vfd_free(vfd);
}

static void vfd_qentry_free_if_empty(struct virtwl_vfd *vfd,
				     struct virtwl_vfd_qentry *qentry)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];

	if (qentry->hdr->type == VIRTIO_WL_CMD_VFD_RECV) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		ssize_t data_len =
			(ssize_t)qentry->len - (ssize_t)sizeof(*recv) -
			(ssize_t)recv->vfd_count * (ssize_t)sizeof(__le32);

		if (qentry->vfd_offset < recv->vfd_count)
			return;

		if ((s64)qentry->data_offset < data_len)
			return;
	}

	mutex_lock(vq_lock);
	vq_return_inbuf_locked(vq, qentry->hdr);
	mutex_unlock(vq_lock);
	list_del(&qentry->list);
	kfree(qentry);
	virtqueue_kick(vq);
}

static ssize_t vfd_out_locked(struct virtwl_vfd *vfd, char __user *buffer,
			      size_t len)
{
	struct virtwl_vfd_qentry *qentry, *next;
	ssize_t read_count = 0;

	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		size_t recv_offset = sizeof(*recv) + recv->vfd_count *
				     sizeof(__le32) + qentry->data_offset;
		u8 *buf = (u8 *)recv + recv_offset;
		ssize_t to_read = (ssize_t)qentry->len - (ssize_t)recv_offset;
		if (read_count >= len)
			break;
		if (to_read <= 0)
			continue;
		if (qentry->hdr->type != VIRTIO_WL_CMD_VFD_RECV)
			continue;

		if ((to_read + read_count) > len)
			to_read = len - read_count;

		if (copy_to_user(buffer + read_count, buf, to_read)) {
			/* return error unless we have some data to return */
			if (read_count == 0)
				read_count = -EFAULT;
			break;
		}

		read_count += to_read;

		qentry->data_offset += to_read;
		vfd_qentry_free_if_empty(vfd, qentry);
	}

	return read_count;
}

/* must hold both vfd->lock and vi->vfds_lock */
static size_t vfd_out_vfds_locked(struct virtwl_vfd *vfd,
				  struct virtwl_vfd **vfds, size_t count)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtwl_vfd_qentry *qentry, *next;
	size_t i;
	size_t read_count = 0;

	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		size_t vfd_offset = sizeof(*recv) + qentry->vfd_offset *
				    sizeof(__le32);
		__le32 *vfds_le = (__le32 *)((void *)recv + vfd_offset);
		ssize_t vfds_to_read = recv->vfd_count - qentry->vfd_offset;
		if (read_count >= count)
			break;
		if (vfds_to_read <= 0)
			continue;
		if (qentry->hdr->type != VIRTIO_WL_CMD_VFD_RECV)
			continue;

		if ((vfds_to_read + read_count) > count)
			vfds_to_read = count - read_count;

		for (i = 0; i < vfds_to_read; i++) {
			uint32_t vfd_id = le32_to_cpu(vfds_le[i]);
			vfds[read_count] = idr_find(&vi->vfds, vfd_id);
			if (vfds[read_count]) {
				read_count++;
			} else {
				printk("virtwl: received a vfd with unrecognized id: %u\n",
				       vfd_id);
			}
			qentry->vfd_offset++;
		}

		vfd_qentry_free_if_empty(vfd, qentry);
	}

	return read_count;
}

/* this can only be called if the caller has unique ownership of the vfd */
static int do_vfd_close(struct virtwl_vfd *vfd)
{
	struct virtio_wl_ctrl_vfd *ctrl_close;
	struct virtwl_info *vi = vfd->vi;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret = 0;

	ctrl_close = kzalloc(sizeof(*ctrl_close), GFP_KERNEL);
	if (!ctrl_close)
		return -ENOMEM;

	ctrl_close->hdr.type = VIRTIO_WL_CMD_VFD_CLOSE;
	ctrl_close->vfd_id = vfd->id;

	sg_init_one(&in_sg, &ctrl_close->hdr, sizeof(struct virtio_wl_ctrl_vfd));
	sg_init_one(&out_sg, &ctrl_close->hdr, sizeof(struct virtio_wl_ctrl_hdr));

	init_completion(&finish_completion);
	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion,
			   false /* block */);
	if (ret) {
		printk("virtwl: failed to queue close vfd id %u: %d\n", vfd->id,
		       ret);
		goto free_ctrl_close;
	}

	wait_for_completion(&finish_completion);
	virtwl_vfd_remove(vfd);

free_ctrl_close:
	kfree(ctrl_close);
	return ret;
}

static ssize_t virtwl_vfd_recv(struct file *filp, char __user *buffer,
			       size_t len, struct virtwl_vfd **vfds,
			       size_t *vfd_count)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	ssize_t read_count = 0;
	size_t vfd_read_count = 0;

	mutex_lock(&vi->vfds_lock);
	mutex_lock(&vfd->lock);

	while (read_count == 0 && vfd_read_count == 0) {
		while (list_empty(&vfd->in_queue)) {
			mutex_unlock(&vfd->lock);
			mutex_unlock(&vi->vfds_lock);
			if (filp->f_flags & O_NONBLOCK)
				return -EAGAIN;

			if (wait_event_interruptible(vfd->in_waitq,
				!list_empty(&vfd->in_queue)))
				return -ERESTARTSYS;

			mutex_lock(&vi->vfds_lock);
			mutex_lock(&vfd->lock);
		}

		read_count = vfd_out_locked(vfd, buffer, len);
		if (read_count < 0)
			goto out_unlock;
		if (vfds && vfd_count && *vfd_count)
			vfd_read_count = vfd_out_vfds_locked(vfd, vfds,
							     *vfd_count);
	}

	*vfd_count = vfd_read_count;

out_unlock:
	mutex_unlock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);
	return read_count;
}

static int virtwl_vfd_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct virtwl_vfd *vfd = filp->private_data;
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	int ret = 0;

	mutex_lock(&vfd->lock);

	if (!(vfd->flags & VIRTIO_WL_VFD_MAP)) {
		ret = -EACCES;
		goto out_unlock;
	}

	if ((vma->vm_flags & VM_WRITE) && !(vfd->flags & VIRTIO_WL_VFD_WRITE)) {
		ret = -EACCES;
		goto out_unlock;
	}

	if (vm_size + (vma->vm_pgoff << PAGE_SHIFT) > PAGE_ALIGN(vfd->size)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = io_remap_pfn_range(vma, vma->vm_start, vfd->pfn, vm_size,
				 vma->vm_page_prot);
	if (ret)
		goto out_unlock;

	vma->vm_flags |= VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

out_unlock:
	mutex_unlock(&vfd->lock);
	return ret;
}

static unsigned int virtwl_vfd_poll(struct file *filp,
				    struct poll_table_struct *wait)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	unsigned int mask = 0;

	mutex_lock(&vi->vq_locks[VIRTWL_VQ_OUT]);
	poll_wait(filp, &vi->out_waitq, wait);
	if (vi->vqs[VIRTWL_VQ_OUT]->num_free)
		mask |= POLLOUT | POLLWRNORM;
	mutex_unlock(&vi->vq_locks[VIRTWL_VQ_OUT]);

	mutex_lock(&vfd->lock);
	poll_wait(filp, &vfd->in_waitq, wait);
	if (!list_empty(&vfd->in_queue))
		mask |= POLLIN | POLLRDNORM;
	mutex_unlock(&vfd->lock);

	return mask;
}

static int virtwl_vfd_release(struct inode *inodep, struct file *filp)
{
	struct virtwl_vfd *vfd = filp->private_data;
	uint32_t vfd_id = vfd->id;

	/*
	 * if release is called, filp must be out of references and we have the
	 * last reference
	 */
	int ret = do_vfd_close(vfd);
	if (ret)
		printk("virtwl: failed to release vfd id %u: %d\n", vfd_id,
		       ret);
	return 0;
}

static int virtwl_open(struct inode *inodep, struct file *filp)
{
	struct virtwl_info *vi = container_of(inodep->i_cdev,
					      struct virtwl_info, cdev);

	filp->private_data = vi;

	return 0;
}

static int do_send(struct virtwl_vfd *vfd, const char __user *buffer, u32 len,
		   int *vfd_fds, bool nonblock)
{
	struct virtwl_info *vi = vfd->vi;
	struct fd vfd_files[VIRTWL_SEND_MAX_ALLOCS] = { { 0 } };
	struct virtwl_vfd *vfds[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	size_t vfd_count = 0;
	size_t post_send_size;
	struct virtio_wl_ctrl_vfd_send *ctrl_send;
	__le32 *vfd_ids;
	u8 *out_buffer;
	unsigned long remaining;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret;
	int i;

	if (vfd_fds) {
		for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++) {
			struct fd vfd_file;
			int fd = vfd_fds[i];
			if (fd < 0)
				break;

			vfd_file = fdget(vfd_fds[i]);
			if (!vfd_file.file) {
				ret = -EBADFD;
				goto put_files;
			}
			vfd_files[i] = vfd_file;

			if (vfd_file.file->f_op != &virtwl_vfd_fops) {
				ret = -EINVAL;
				goto put_files;
			}

			vfds[i] = vfd_file.file->private_data;
			if (!vfds[i] || !vfds[i]->id) {
				ret = -EINVAL;
				goto put_files;
			}

			vfd_count++;
		}
	}

	post_send_size = vfd_count * sizeof(__le32) + len;
	ctrl_send = kzalloc(sizeof(*ctrl_send) + post_send_size, GFP_KERNEL);
	if (!ctrl_send) {
		ret = -ENOMEM;
		goto put_files;
	}

	vfd_ids = (__le32 *)((u8*)ctrl_send + sizeof(*ctrl_send));
	out_buffer = (u8*)vfd_ids + vfd_count * sizeof(__le32);

	ctrl_send->hdr.type = VIRTIO_WL_CMD_VFD_SEND;
	ctrl_send->vfd_id = vfd->id;
	ctrl_send->vfd_count = vfd_count;
	for (i = 0; i < vfd_count; i++) {
		vfd_ids[i] = cpu_to_le32(vfds[i]->id);
	}

	remaining = copy_from_user(out_buffer, buffer, len);
	if (remaining)
		goto free_ctrl_send;

	init_completion(&finish_completion);
	sg_init_one(&out_sg, ctrl_send, sizeof(*ctrl_send) + post_send_size);
	sg_init_one(&in_sg, ctrl_send, sizeof(struct virtio_wl_ctrl_hdr));

	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion, nonblock);
	if (ret)
		goto free_ctrl_send;

	wait_for_completion(&finish_completion);

	ret = virtwl_resp_err(ctrl_send->hdr.type);

free_ctrl_send:
	kfree(ctrl_send);
put_files:
	for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++) {
		if (!vfd_files[i].file)
			continue;
		fdput(vfd_files[i]);
	}
	return ret;
}

static struct virtwl_vfd *do_new(struct virtwl_info *vi, uint32_t type,
				 uint32_t size, bool nonblock)
{
	struct virtio_wl_ctrl_vfd_new *ctrl_new;
	struct virtwl_vfd *vfd;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret = 0;

	if (type != VIRTWL_IOCTL_NEW_CTX && type != VIRTWL_IOCTL_NEW_ALLOC)
		return ERR_PTR(-EINVAL);

	ctrl_new = kzalloc(sizeof(*ctrl_new), GFP_KERNEL);
	if (!ctrl_new)
		return ERR_PTR(-ENOMEM);

	vfd = virtwl_vfd_alloc(vi);
	if (!vfd) {
		ret = -ENOMEM;
		goto free_ctrl_new;
	}

	/*
	 * Take the lock before adding it to the vfds list where others might
	 * reference it.
	 */
	mutex_lock(&vfd->lock);

	mutex_lock(&vi->vfds_lock);
	ret = idr_alloc(&vi->vfds, vfd, 1, VIRTWL_MAX_ALLOC, GFP_KERNEL);
	mutex_unlock(&vi->vfds_lock);
	if (ret <= 0)
		goto free_vfd;

	vfd->id = ret;
	ret = 0;

	ctrl_new->vfd_id = vfd->id;
	switch (type) {
	case VIRTWL_IOCTL_NEW_CTX:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_CTX;
		ctrl_new->flags = VIRTIO_WL_VFD_CONTROL;
		ctrl_new->size = 0;
		break;
	case VIRTWL_IOCTL_NEW_ALLOC:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW;
		ctrl_new->flags = VIRTIO_WL_VFD_WRITE | VIRTIO_WL_VFD_MAP;
		ctrl_new->size = size;
		break;
	default:
		ret = -EINVAL;
		goto remove_vfd;
	}

	init_completion(&finish_completion);
	sg_init_one(&out_sg, ctrl_new, sizeof(*ctrl_new));
	sg_init_one(&in_sg, ctrl_new, sizeof(*ctrl_new));

	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion, nonblock);
	if (ret)
		goto remove_vfd;

	wait_for_completion(&finish_completion);

	ret = virtwl_resp_err(ctrl_new->hdr.type);
	if (ret)
		goto remove_vfd;

	vfd->size = ctrl_new->size;
	vfd->pfn = ctrl_new->pfn;
	vfd->flags = ctrl_new->flags;

	mutex_unlock(&vfd->lock);

	kfree(ctrl_new);
	return vfd;

remove_vfd:
	/* unlock the vfd to avoid deadlock when unlinking it */
	mutex_unlock(&vfd->lock);
	virtwl_vfd_lock_unlink(vfd);
free_vfd:
	virtwl_vfd_free(vfd);
free_ctrl_new:
	kfree(ctrl_new);
	return ERR_PTR(ret);
}

static long virtwl_ioctl_send(struct file *filp, void __user *ptr)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_ioctl_txn ioctl_send;
	void __user *user_data = ptr + sizeof(struct virtwl_ioctl_txn);
	int ret;

	ret = copy_from_user(&ioctl_send, ptr, sizeof(struct virtwl_ioctl_txn));
	if (ret)
		return -EFAULT;

	/* Early check for user error; do_send still uses copy_from_user. */
	ret = !access_ok(VERIFY_READ, user_data, ioctl_send.len);
	if (ret)
		return -EFAULT;

	return do_send(vfd, user_data, ioctl_send.len, ioctl_send.fds,
		       filp->f_flags & O_NONBLOCK);
}

/* Start import of google file.c functions */

static inline void __clear_open_fd(int fd, struct fdtable *fdt)
{
	__clear_bit(fd, fdt->open_fds);
}


static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	struct fdtable *fdt = files_fdtable(files);
	__clear_open_fd(fd, fdt);
	if (fd < files->next_fd)
		files->next_fd = fd;
}


static inline void __clear_close_on_exec(int fd, struct fdtable *fdt)
{
	__clear_bit(fd, fdt->close_on_exec);
}

int __close_fd(struct files_struct *files, unsigned fd)
{
	struct file *file;
	struct fdtable *fdt;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (fd >= fdt->max_fds)
		goto out_unlock;
	file = fdt->fd[fd];
	if (!file)
		goto out_unlock;
	rcu_assign_pointer(fdt->fd[fd], NULL);
	__clear_close_on_exec(fd, fdt);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
	return filp_close(file, files);

out_unlock:
	spin_unlock(&files->file_lock);
	return -EBADF;
}

static long virtwl_ioctl_recv(struct file *filp, void __user *ptr)
{
	struct virtwl_ioctl_txn ioctl_recv;
	void __user *user_data = ptr + sizeof(struct virtwl_ioctl_txn);
	int __user *user_fds = (int __user *)ptr;
	size_t vfd_count = VIRTWL_SEND_MAX_ALLOCS;
	struct virtwl_vfd *vfds[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	int fds[VIRTWL_SEND_MAX_ALLOCS];
	size_t i;
	int ret = 0;


	for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++)
		fds[i] = -1;

	ret = copy_from_user(&ioctl_recv, ptr, sizeof(struct virtwl_ioctl_txn));
	if (ret)
		return -EFAULT;

	/* Early check for user error. */
	ret = !access_ok(VERIFY_WRITE, user_data, ioctl_recv.len);
	if (ret)
		return -EFAULT;

	ret = virtwl_vfd_recv(filp, user_data, ioctl_recv.len, vfds,
			      &vfd_count);
	if (ret < 0)
		return ret;

	ret = copy_to_user(&((struct virtwl_ioctl_txn __user *)ptr)->len, &ret,
			   sizeof(ioctl_recv.len));
	if (ret) {
		ret = -EFAULT;
		goto free_vfds;
	}

	for (i = 0; i < vfd_count; i++) {
		ret = anon_inode_getfd("[virtwl_vfd]", &virtwl_vfd_fops,
				       vfds[i], O_CLOEXEC | O_RDWR);
		if (ret < 0) {
			do_vfd_close(vfds[i]);
			goto free_vfds;
		}
		vfds[i] = NULL;
		fds[i] = ret;
	}

	ret = copy_to_user(user_fds, fds, sizeof(int) * VIRTWL_SEND_MAX_ALLOCS);
	if (ret) {
		ret = -EFAULT;
		goto free_vfds;
	}

	return 0;

free_vfds:
	for (i = 0; i < vfd_count; i++) {
		if (vfds[i])
			do_vfd_close(vfds[i]);
		if (fds[i] >= 0)
			__close_fd(current->files, fds[i]);
	}
	return ret;
}

static long virtwl_vfd_ioctl(struct file *filp, unsigned int cmd,
			     void __user *ptr)
{
	switch (cmd) {
	case VIRTWL_IOCTL_SEND:
		return virtwl_ioctl_send(filp, ptr);
	case VIRTWL_IOCTL_RECV:
		return virtwl_ioctl_recv(filp, ptr);
	default:
		return -ENOTTY;
	}
}

static long virtwl_ioctl_new(struct file *filp, void __user *ptr)
{
	struct virtwl_info *vi = filp->private_data;
	struct virtwl_vfd *vfd;
	struct virtwl_ioctl_new ioctl_new;
	int ret;

	/* Early check for user error. */
	ret = !access_ok(VERIFY_WRITE, ptr, sizeof(struct virtwl_ioctl_new));
	if (ret)
		return -EFAULT;

	ret = copy_from_user(&ioctl_new, ptr, sizeof(struct virtwl_ioctl_new));
	if (ret)
		return -EFAULT;

	ioctl_new.size = PAGE_ALIGN(ioctl_new.size);

	vfd = do_new(vi, ioctl_new.type, ioctl_new.size,
		     filp->f_flags & O_NONBLOCK);
	if (IS_ERR(vfd))
		return PTR_ERR(vfd);

	ret = anon_inode_getfd("[virtwl_vfd]", &virtwl_vfd_fops, vfd,
			       O_CLOEXEC | O_RDWR);
	if (ret < 0) {
		do_vfd_close(vfd);
		return ret;
	}

	ioctl_new.fd = ret;
	ret = copy_to_user(ptr, &ioctl_new, sizeof(struct virtwl_ioctl_new));
	if (ret) {
		/* The release operation will handle freeing this alloc */
		sys_close(ioctl_new.fd);
		return -EFAULT;
	}

	return 0;
}

static long virtwl_ioctl_ptr(struct file *filp, unsigned int cmd,
			     void __user *ptr)
{
	if (filp->f_op == &virtwl_vfd_fops)
		return virtwl_vfd_ioctl(filp, cmd, ptr);

	switch (cmd) {
	case VIRTWL_IOCTL_NEW:
		return virtwl_ioctl_new(filp, ptr);
	default:
		return -ENOTTY;
	}
}

static long virtwl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return virtwl_ioctl_ptr(filp, cmd, (void __user *)arg);
}

#ifdef CONFIG_COMPAT
static long virtwl_ioctl_compat(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	return virtwl_ioctl_ptr(filp, cmd, compat_ptr(arg));
}
#else
#define virtwl_ioctl_compat NULL
#endif

static int virtwl_release(struct inode *inodep, struct file *filp)
{
	return 0;
}

static struct file_operations virtwl_fops =
{
	.open = virtwl_open,
	.unlocked_ioctl = virtwl_ioctl,
	.compat_ioctl = virtwl_ioctl_compat,
	.release = virtwl_release,
};

static struct file_operations virtwl_vfd_fops =
{
	.mmap = virtwl_vfd_mmap,
	.poll = virtwl_vfd_poll,
	.unlocked_ioctl = virtwl_ioctl,
	.compat_ioctl = virtwl_ioctl_compat,
	.release = virtwl_vfd_release,
};

static int probe_common(struct virtio_device *vdev)
{
	int i;
	int ret;
	struct virtwl_info *vi = NULL;
	vq_callback_t *vq_callbacks[] = { vq_in_cb, vq_out_cb };
	const char *vq_names[] = { "in", "out" };

	vi = kzalloc(sizeof(struct virtwl_info), GFP_KERNEL);
	if (!vi) {
		printk("virtwl: failed to alloc virtwl_info struct\n");
		return -ENOMEM;
	}

	vdev->priv = vi;

	ret = alloc_chrdev_region(&vi->dev_num, 0, 1, "wl");
	if (ret) {
		ret = -ENOMEM;
		printk("virtwl: failed to allocate wl chrdev region: %d\n",
		       ret);
		goto free_vi;
	}

	vi->class = class_create(THIS_MODULE, "wl");
	if (IS_ERR(vi->class)) {
		ret = PTR_ERR(vi->class);
		printk("virtwl: failed to create wl class: %d\n", ret);
		goto unregister_region;

	}

	vi->dev = device_create(vi->class, NULL, vi->dev_num, vi, "wl%d", 0);
	if (IS_ERR(vi->dev)) {
		ret = PTR_ERR(vi->dev);
		printk("virtwl: failed to create wl0 device: %d\n", ret);
		goto destroy_class;
	}

	cdev_init(&vi->cdev, &virtwl_fops);
	ret = cdev_add(&vi->cdev, vi->dev_num, 1);
	if (ret) {
		printk("virtwl: failed to add virtio wayland character device to system: %d\n",
		       ret);
		goto destroy_device;
	}

	for (i = 0; i < VIRTWL_QUEUE_COUNT; i++)
		mutex_init(&vi->vq_locks[i]);

	ret = vdev->config->find_vqs(vdev, VIRTWL_QUEUE_COUNT, vi->vqs,
				     vq_callbacks, vq_names);
	if (ret) {
		printk("virtwl: failed to find virtio wayland queues: %d\n",
		       ret);
		goto del_cdev;
	}

	INIT_WORK(&vi->in_vq_work, vq_in_work_handler);
	INIT_WORK(&vi->out_vq_work, vq_out_work_handler);
	init_waitqueue_head(&vi->out_waitq);

	mutex_init(&vi->vfds_lock);
	idr_init(&vi->vfds);

	/* lock is unneeded as we have unique ownership */
	ret = vq_fill_locked(vi->vqs[VIRTWL_VQ_IN]);
	if (ret) {
		printk("virtwl: failed to fill in virtqueue: %d", ret);
		goto del_cdev;
	}

	virtio_device_ready(vdev);
	virtqueue_kick(vi->vqs[VIRTWL_VQ_IN]);


	return 0;

del_cdev:
	cdev_del(&vi->cdev);
destroy_device:
	put_device(vi->dev);
destroy_class:
	class_destroy(vi->class);
unregister_region:
	unregister_chrdev_region(vi->dev_num, 0);
free_vi:
	kfree(vi);
	return ret;
}

static void remove_common(struct virtio_device *vdev)
{
	struct virtwl_info *vi = vdev->priv;

	cdev_del(&vi->cdev);
	put_device(vi->dev);
	class_destroy(vi->class);
	unregister_chrdev_region(vi->dev_num, 0);
	kfree(vi);
}

static int virtwl_probe(struct virtio_device *vdev)
{
	return probe_common(vdev);
}

static void virtwl_remove(struct virtio_device *vdev)
{
	remove_common(vdev);
}

static void virtwl_scan(struct virtio_device *vdev)
{
}


static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_WL, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_wl_driver = {
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtwl_probe,
	.remove =	virtwl_remove,
	.scan =		virtwl_scan,
};

module_virtio_driver(virtio_wl_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio wayland driver");
MODULE_LICENSE("GPL");
