// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022-2024, Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/iommu.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_accel.h>
#include <drm/drm_managed.h>
#include "drm_local/amdxdna_accel.h"

#include "amdxdna_drv.h"
#include "amdxdna_sysfs.h"
#ifdef AMDXDNA_DEVEL
#include "amdxdna_devel.h"
#endif

#define CREATE_TRACE_POINTS
#include "amdxdna_trace.h"

/*
 *  There are platforms which share the same PCI device ID
 *  but have different PCI revision IDs. So, let the PCI class
 *  determine the probe and later use the (device_id, rev_id)
 *  pair as a key to select the devices.
 */
static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_ANY_ID),
		.class = PCI_CLASS_SP_OTHER << 8,  /* Signal Processing */
		.class_mask = 0xFFFF00,
	},
	{0}
};

MODULE_DEVICE_TABLE(pci, pci_ids);

static const struct amdxdna_device_id amdxdna_ids[] = {
	{ 0x1502, 0x0,  &dev_npu1_info },
	{ 0x17f0, 0x0,  &dev_npu2_info },
	{ 0x17f0, 0x10, &dev_npu4_info },
	{0}
};

static int amdxdna_drm_open(struct drm_device *ddev, struct drm_file *filp)
{
	struct amdxdna_dev *xdna = to_xdna_dev(ddev);
	struct amdxdna_client *client;
	int ret;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	client->pid = pid_nr(filp->pid);
	client->xdna = xdna;

#ifdef AMDXDNA_DEVEL
	if (iommu_mode != AMDXDNA_IOMMU_PASID)
		goto skip_sva_bind;
#endif
	client->sva = iommu_sva_bind_device(xdna->ddev.dev, current->mm);
	if (IS_ERR(client->sva)) {
		ret = PTR_ERR(client->sva);
		XDNA_ERR(xdna, "SVA bind device failed, ret %d", ret);
		goto failed;
	}
	client->pasid = iommu_sva_get_pasid(client->sva);
	if (client->pasid == IOMMU_PASID_INVALID) {
		XDNA_ERR(xdna, "SVA get pasid failed");
		ret = -ENODEV;
		goto unbind_sva;
	}
#ifdef AMDXDNA_DEVEL
skip_sva_bind:
#endif
	mutex_init(&client->hwctx_lock);
	init_srcu_struct(&client->hwctx_srcu);
	idr_init(&client->hwctx_idr);
	mutex_init(&client->mm_lock);
	client->dev_heap = AMDXDNA_INVALID_BO_HANDLE;

	mutex_lock(&xdna->dev_lock);
	list_add_tail(&client->node, &xdna->client_list);
	mutex_unlock(&xdna->dev_lock);

	filp->driver_priv = client;
	client->filp = filp;

	XDNA_DBG(xdna, "pid %d opened", client->pid);
	return 0;

unbind_sva:
	iommu_sva_unbind_device(client->sva);
failed:
	kfree(client);

	return ret;
}

static void amdxdna_drm_close(struct drm_device *ddev, struct drm_file *filp)
{
	struct amdxdna_client *client = filp->driver_priv;
	struct amdxdna_dev *xdna = to_xdna_dev(ddev);

	XDNA_DBG(xdna, "closing pid %d", client->pid);

	idr_destroy(&client->hwctx_idr);
	cleanup_srcu_struct(&client->hwctx_srcu);
	mutex_destroy(&client->hwctx_lock);
	mutex_destroy(&client->mm_lock);

#ifdef AMDXDNA_DEVEL
	if (iommu_mode != AMDXDNA_IOMMU_PASID)
		goto skip_sva_unbind;
#endif
	iommu_sva_unbind_device(client->sva);
#ifdef AMDXDNA_DEVEL
skip_sva_unbind:
#endif

	XDNA_DBG(xdna, "pid %d closed", client->pid);
	kfree(client);
}

static int amdxdna_flush(struct file *f, fl_owner_t id)
{
	struct drm_file *filp = f->private_data;
	struct amdxdna_client *client = filp->driver_priv;
	struct amdxdna_dev *xdna = client->xdna;

	XDNA_DBG(xdna, "pid %d flushing...", client->pid);
	mutex_lock(&xdna->dev_lock);
	list_del(&client->node);
	amdxdna_hwctx_remove_all(client);
	mutex_unlock(&xdna->dev_lock);

	return 0;
}

static int amdxdna_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *drm_filp = filp->private_data;
	struct amdxdna_client *client = drm_filp->driver_priv;
	struct amdxdna_dev *xdna = client->xdna;

	if (likely(vma->vm_pgoff >= DRM_FILE_PAGE_OFFSET_START))
		return drm_gem_mmap(filp, vma);

	if (!xdna->dev_info->ops->mmap)
		return -EOPNOTSUPP;

	return xdna->dev_info->ops->mmap(xdna, vma);
}

static int amdxdna_drm_get_info_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct amdxdna_drm_get_info *args = data;
	struct amdxdna_dev *xdna = to_xdna_dev(dev);

	if (!xdna->dev_info->ops->get_info)
		return -EOPNOTSUPP;

	XDNA_DBG(xdna, "Request parameter %u", args->param);
	return xdna->dev_info->ops->get_info(xdna, args);
}

static const struct drm_ioctl_desc amdxdna_drm_ioctls[] = {
	/* Context */
	DRM_IOCTL_DEF_DRV(AMDXDNA_CREATE_HWCTX, amdxdna_drm_create_hwctx_ioctl, 0),
	DRM_IOCTL_DEF_DRV(AMDXDNA_DESTROY_HWCTX, amdxdna_drm_destroy_hwctx_ioctl, 0),
	DRM_IOCTL_DEF_DRV(AMDXDNA_CONFIG_HWCTX, amdxdna_drm_config_hwctx_ioctl, 0),
	/* BO */
	DRM_IOCTL_DEF_DRV(AMDXDNA_CREATE_BO, amdxdna_drm_create_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(AMDXDNA_GET_BO_INFO, amdxdna_drm_get_bo_info_ioctl, 0),
	DRM_IOCTL_DEF_DRV(AMDXDNA_SYNC_BO, amdxdna_drm_sync_bo_ioctl, 0),
	/* Exectuion */
	DRM_IOCTL_DEF_DRV(AMDXDNA_EXEC_CMD, amdxdna_drm_exec_cmd_ioctl, 0),
	DRM_IOCTL_DEF_DRV(AMDXDNA_WAIT_CMD, amdxdna_drm_wait_cmd_ioctl, 0),
	/* Query */
	DRM_IOCTL_DEF_DRV(AMDXDNA_GET_INFO, amdxdna_drm_get_info_ioctl, 0),
};

static const struct file_operations amdxdna_fops = {
	.owner		= THIS_MODULE,
	.open		= accel_open,
	.release	= drm_release,
	.flush		= amdxdna_flush,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= noop_llseek,
	.mmap		= amdxdna_drm_gem_mmap
};

static const struct drm_driver amdxdna_drm_drv = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.fops = &amdxdna_fops,
	.name = "amdxdna_accel_driver",
	.desc = "AMD XDNA DRM implementation",
	.date = "20240124",
	.major = AMDXDNA_DRIVER_MAJOR,
	.minor = AMDXDNA_DRIVER_MINOR,
	.open = amdxdna_drm_open,
	.postclose = amdxdna_drm_close,
	.ioctls = amdxdna_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(amdxdna_drm_ioctls),

	/* For shmem object create */
	.gem_create_object = amdxdna_gem_create_object,
};

static const struct amdxdna_dev_info *
amdxdna_get_dev_info(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(amdxdna_ids); i++) {
		if (pdev->device == amdxdna_ids[i].device &&
		    pdev->revision == amdxdna_ids[i].revision)
			return amdxdna_ids[i].dev_info;
	}
	return NULL;
}

static int amdxdna_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct amdxdna_dev *xdna;
	int ret;

	xdna = devm_drm_dev_alloc(&pdev->dev, &amdxdna_drm_drv, typeof(*xdna), ddev);
	if (IS_ERR(xdna))
		return PTR_ERR(xdna);

	xdna->dev_info = amdxdna_get_dev_info(pdev);
	if (!xdna->dev_info)
		return -ENODEV;

	drmm_mutex_init(&xdna->ddev, &xdna->dev_lock);
	INIT_LIST_HEAD(&xdna->client_list);
	pci_set_drvdata(pdev, xdna);

	if (!xdna->dev_info->ops->init || !xdna->dev_info->ops->fini)
		return -EOPNOTSUPP;

	mutex_lock(&xdna->dev_lock);
	ret = xdna->dev_info->ops->init(xdna);
	mutex_unlock(&xdna->dev_lock);
	if (ret) {
		XDNA_ERR(xdna, "Hardware init failed, ret %d", ret);
		return ret;
	}

	ret = amdxdna_sysfs_init(xdna);
	if (ret) {
		XDNA_ERR(xdna, "Create amdxdna attrs failed: %d", ret);
		goto failed_npu_fini;
	}

	ret = drm_dev_register(&xdna->ddev, 0);
	if (ret) {
		XDNA_ERR(xdna, "DRM register failed, ret %d", ret);
		goto failed_sysfs_fini;
	}

	/* Debug fs needs to go after register DRM dev */
	if (xdna->dev_info->ops->debugfs)
		xdna->dev_info->ops->debugfs(xdna);

	return 0;

failed_sysfs_fini:
	amdxdna_sysfs_fini(xdna);
failed_npu_fini:
	mutex_lock(&xdna->dev_lock);
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
	return ret;
}

static void amdxdna_remove(struct pci_dev *pdev)
{
	struct amdxdna_dev *xdna = pci_get_drvdata(pdev);
	struct amdxdna_client *client, *tmp;

	drm_dev_unplug(&xdna->ddev);
	amdxdna_sysfs_fini(xdna);

	mutex_lock(&xdna->dev_lock);
	list_for_each_entry_safe(client, tmp, &xdna->client_list, node)
		amdxdna_hwctx_remove_all(client);

	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
}

static int amdxdna_pmops_suspend(struct device *dev)
{
	struct amdxdna_dev *xdna = pci_get_drvdata(to_pci_dev(dev));
	struct amdxdna_client *client;

	mutex_lock(&xdna->dev_lock);
	list_for_each_entry(client, &xdna->client_list, node)
		amdxdna_hwctx_suspend(client);

	if (xdna->dev_info->ops->suspend)
		xdna->dev_info->ops->suspend(xdna);
	mutex_unlock(&xdna->dev_lock);

	return 0;
}

static int amdxdna_pmops_resume(struct device *dev)
{
	struct amdxdna_dev *xdna = pci_get_drvdata(to_pci_dev(dev));
	struct amdxdna_client *client;
	int ret;

	XDNA_INFO(xdna, "firmware resuming...");
	mutex_lock(&xdna->dev_lock);
	if (xdna->dev_info->ops->resume) {
		ret = xdna->dev_info->ops->resume(xdna);
		if (ret) {
			XDNA_ERR(xdna, "resume NPU firmware failed");
			mutex_unlock(&xdna->dev_lock);
			return ret;
		}
	}

	XDNA_INFO(xdna, "hardware context resuming...");
	list_for_each_entry(client, &xdna->client_list, node)
		amdxdna_hwctx_resume(client);
	mutex_unlock(&xdna->dev_lock);

	return 0;
}

static const struct dev_pm_ops amdxdna_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(amdxdna_pmops_suspend, amdxdna_pmops_resume)
};

static struct pci_driver amdxdna_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = pci_ids,
	.probe = amdxdna_probe,
	.remove = amdxdna_remove,
	.driver.pm = &amdxdna_pm_ops,
};

module_pci_driver(amdxdna_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_VERSION("0.1");
MODULE_DESCRIPTION("amdxdna driver");
