/*
 * Copyright (c) 2015 Cray Inc. All rights reserved.
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * CNTR common code
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "gnix.h"
#include "gnix_cntr.h"
#include "gnix_nic.h"

/*******************************************************************************
 * Forward declarations for filling functions.
 ******************************************************************************/

/*******************************************************************************
 * Forward declarations for ops structures.
 ******************************************************************************/
static struct fi_ops gnix_cntr_fi_ops;
static struct fi_ops_cntr gnix_cntr_ops;

/*******************************************************************************
 * Internal helper functions
 ******************************************************************************/

static int __verify_cntr_attr(struct fi_cntr_attr *attr)
{
	int ret = FI_SUCCESS;

	GNIX_TRACE(FI_LOG_CQ, "\n");

	if (!attr)
		return -FI_EINVAL;

	if (attr->events != FI_CNTR_EVENTS_COMP) {
		GNIX_WARN(FI_LOG_CQ, "cntr event type: %d unsupported.\n",
			  attr->events);
		return -FI_EINVAL;
	}

	/*
	 * TODO: need to support wait objects on cntr
	 */
	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
	case FI_WAIT_NONE:
		break;
	case FI_WAIT_SET:
	case FI_WAIT_FD:
	case FI_WAIT_MUTEX_COND:
	default:
		GNIX_WARN(FI_LOG_CQ, "wait type: %d unsupported.\n",
			  attr->wait_obj);
		return -FI_EINVAL;
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static int __gnix_cntr_progress(struct gnix_fid_cntr *cntr)
{
	struct gnix_cq_poll_nic *pnic, *tmp;
	int rc = FI_SUCCESS;

	rwlock_rdlock(&cntr->nic_lock);

	dlist_for_each_safe(&cntr->poll_nics, pnic, tmp, list) {
		rc = _gnix_nic_progress(pnic->nic);
		if (rc) {
			GNIX_WARN(FI_LOG_CQ,
				  "_gnix_nic_progress failed: %d\n", rc);
		}
	}

	rwlock_unlock(&cntr->nic_lock);

	return rc;
}

/*******************************************************************************
 * Exposed helper functions
 ******************************************************************************/

int _gnix_cntr_inc(struct gnix_fid_cntr *cntr)
{
	if (cntr == NULL)
		return -FI_EINVAL;

	atomic_inc(&cntr->cnt);

	if (cntr->wait)
		_gnix_signal_wait_obj(cntr->wait);

	return FI_SUCCESS;
}

int _gnix_cntr_inc_err(struct gnix_fid_cntr *cntr)
{
	if (cntr == NULL)
		return -FI_EINVAL;

	atomic_inc(&cntr->cnt_err);

	if (cntr->wait)
		_gnix_signal_wait_obj(cntr->wait);

	return FI_SUCCESS;
}

int _gnix_cntr_poll_nic_add(struct gnix_fid_cntr *cntr, struct gnix_nic *nic)
{
	struct gnix_cntr_poll_nic *pnic, *tmp;

	rwlock_wrlock(&cntr->nic_lock);

	dlist_for_each_safe(&cntr->poll_nics, pnic, tmp, list) {
		if (pnic->nic == nic) {
			pnic->ref_cnt++;
			rwlock_unlock(&cntr->nic_lock);
			return FI_SUCCESS;
		}
	}

	pnic = malloc(sizeof(struct gnix_cntr_poll_nic));
	if (!pnic) {
		GNIX_WARN(FI_LOG_CQ, "Failed to add NIC to CNTR poll list.\n");
		rwlock_unlock(&cntr->nic_lock);
		return -FI_ENOMEM;
	}

	/* EP holds a ref count on the NIC */
	pnic->nic = nic;
	pnic->ref_cnt = 1;
	dlist_init(&pnic->list);
	dlist_insert_tail(&pnic->list, &cntr->poll_nics);

	rwlock_unlock(&cntr->nic_lock);

	GNIX_INFO(FI_LOG_CQ, "Added NIC(%p) to CNTR(%p) poll list\n",
		  nic, cntr);

	return FI_SUCCESS;
}

int _gnix_cntr_poll_nic_rem(struct gnix_fid_cntr *cntr, struct gnix_nic *nic)
{
	struct gnix_cntr_poll_nic *pnic, *tmp;

	rwlock_wrlock(&cntr->nic_lock);

	dlist_for_each_safe(&cntr->poll_nics, pnic, tmp, list) {
		if (pnic->nic == nic) {
			if (!--pnic->ref_cnt) {
				dlist_remove(&pnic->list);
				free(pnic);
				GNIX_INFO(FI_LOG_CQ,
					  "Removed NIC(%p) from CNTR(%p) poll list\n",
					  nic, cntr);
			}
			rwlock_unlock(&cntr->nic_lock);
			return FI_SUCCESS;
		}
	}

	rwlock_unlock(&cntr->nic_lock);

	GNIX_WARN(FI_LOG_CQ, "NIC not found on CNTR poll list.\n");
	return -FI_EINVAL;
}

/*******************************************************************************
 * API functions.
 ******************************************************************************/

static int gnix_cntr_wait(struct fid_cntr *cntr, uint64_t threshold,
			  int timeout)
{
	return -FI_ENOSYS;
}

static int gnix_cntr_close(fid_t fid)
{
	struct gnix_fid_cntr *cntr;

	GNIX_TRACE(FI_LOG_CQ, "\n");

	cntr = container_of(fid, struct gnix_fid_cntr, cntr_fid);
	if (atomic_get(&cntr->ref_cnt) != 0) {
		GNIX_INFO(FI_LOG_CQ, "CNTR ref count: %d, not closing.\n",
			  cntr->ref_cnt);
		return -FI_EBUSY;
	}

	atomic_dec(&cntr->domain->ref_cnt);
	assert(atomic_get(&cntr->domain->ref_cnt) >= 0);

	switch (cntr->attr.wait_obj) {
	case FI_WAIT_NONE:
		break;
	case FI_WAIT_SET:
		_gnix_wait_set_remove(cntr->wait, &cntr->cntr_fid.fid);
		break;
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_MUTEX_COND:
		assert(cntr->wait);
		gnix_wait_close(&cntr->wait->fid);
		break;
	default:
		GNIX_WARN(FI_LOG_CQ, "format: %d unsupported\n.",
			  cntr->attr.wait_obj);
		break;
	}

	free(cntr);

	return FI_SUCCESS;
}

static uint64_t gnix_cntr_readerr(struct fid_cntr *cntr)
{
	int v, ret;
	struct gnix_fid_cntr *cntr_priv;

	if (cntr == NULL)
		return -FI_EINVAL;

	cntr_priv = container_of(cntr, struct gnix_fid_cntr, cntr_fid);
	v = atomic_get(&cntr_priv->cnt_err);

	ret = __gnix_cntr_progress(cntr_priv);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_CQ, " __gnix_cntr_progress returned %d.\n",
			  ret);

	return (uint64_t)v;
}

static uint64_t gnix_cntr_read(struct fid_cntr *cntr)
{
	int v, ret;
	struct gnix_fid_cntr *cntr_priv;

	if (cntr == NULL)
		return -FI_EINVAL;

	cntr_priv = container_of(cntr, struct gnix_fid_cntr, cntr_fid);
	v = atomic_get(&cntr_priv->cnt);

	ret = __gnix_cntr_progress(cntr_priv);
	if (ret != FI_SUCCESS)
		GNIX_WARN(FI_LOG_CQ, " __gnix_cntr_progress returned %d.\n",
			  ret);

	return (uint64_t)v;
}

static int gnix_cntr_add(struct fid_cntr *cntr, uint64_t value)
{
	struct gnix_fid_cntr *cntr_priv;

	if (cntr == NULL)
		return -FI_EINVAL;

	cntr_priv = container_of(cntr, struct gnix_fid_cntr, cntr_fid);
	atomic_add(&cntr_priv->cnt, (int)value);

	if (cntr_priv->wait)
		_gnix_signal_wait_obj(cntr_priv->wait);

	return FI_SUCCESS;
}

static int gnix_cntr_set(struct fid_cntr *cntr, uint64_t value)
{
	struct gnix_fid_cntr *cntr_priv;

	if (cntr == NULL)
		return -FI_EINVAL;

	cntr_priv = container_of(cntr, struct gnix_fid_cntr, cntr_fid);
	atomic_set(&cntr_priv->cnt, (int)value);

	if (cntr_priv->wait)
		_gnix_signal_wait_obj(cntr_priv->wait);

	return FI_SUCCESS;
}

static int gnix_cntr_control(struct fid *cntr, int command, void *arg)
{
	struct gnix_fid_cntr *cntr_priv;

	if (cntr == NULL)
		return -FI_EINVAL;

	cntr_priv = container_of(cntr, struct gnix_fid_cntr, cntr_fid);

	switch (command) {
	case FI_SETOPSFLAG:
		cntr_priv->attr.flags = *(uint64_t *)arg;
		break;
	case FI_GETOPSFLAG:
		if (!arg)
			return -FI_EINVAL;
		*(uint64_t *)arg = cntr_priv->attr.flags;
		break;
	case FI_GETWAIT:
		return _gnix_get_wait_obj(cntr_priv->wait, arg);
	default:
		return -FI_EINVAL;
	}

	return FI_SUCCESS;

}


int gnix_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
		 struct fid_cntr **cntr, void *context)
{
	int ret = FI_SUCCESS;
	struct gnix_fid_domain *domain_priv;
	struct gnix_fid_cntr *cntr_priv;

	GNIX_TRACE(FI_LOG_CQ, "\n");

	ret = __verify_cntr_attr(attr);
	if (ret)
		goto err;

	domain_priv = container_of(domain, struct gnix_fid_domain, domain_fid);
	if (!domain_priv) {
		ret = -FI_EINVAL;
		goto err;
	}

	cntr_priv = calloc(1, sizeof(*cntr_priv));
	if (!cntr_priv) {
		ret = -FI_ENOMEM;
		goto err;
	}

	cntr_priv->domain = domain_priv;
	cntr_priv->attr = *attr;
	atomic_initialize(&cntr_priv->ref_cnt, 0);
	atomic_inc(&cntr_priv->domain->ref_cnt);
	dlist_init(&cntr_priv->poll_nics);
	rwlock_init(&cntr_priv->nic_lock);

	cntr_priv->cntr_fid.fid.fclass = FI_CLASS_CNTR;
	cntr_priv->cntr_fid.fid.context = context;
	cntr_priv->cntr_fid.fid.ops = &gnix_cntr_fi_ops;
	cntr_priv->cntr_fid.ops = &gnix_cntr_ops;

	*cntr = &cntr_priv->cntr_fid;

err:
	return ret;
}


/*******************************************************************************
 * FI_OPS_* data structures.
 ******************************************************************************/
static struct fi_ops gnix_cntr_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = gnix_cntr_close,
	.bind = fi_no_bind,
	.control = gnix_cntr_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_cntr gnix_cntr_ops = {
	.size = sizeof(struct fi_ops_cntr),
	.readerr = gnix_cntr_readerr,
	.read = gnix_cntr_read,
	.add = gnix_cntr_add,
	.set = gnix_cntr_set,
	.wait = gnix_cntr_wait,
};