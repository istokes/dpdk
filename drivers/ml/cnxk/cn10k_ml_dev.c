/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 Marvell.
 */

#include <rte_common.h>
#include <rte_dev.h>
#include <rte_devargs.h>
#include <rte_kvargs.h>
#include <rte_mldev.h>
#include <rte_mldev_pmd.h>
#include <rte_pci.h>

#include <roc_api.h>

#include <eal_firmware.h>

#include "cn10k_ml_dev.h"
#include "cn10k_ml_ops.h"

#define CN10K_ML_FW_PATH		"fw_path"
#define CN10K_ML_FW_ENABLE_DPE_WARNINGS "enable_dpe_warnings"
#define CN10K_ML_FW_REPORT_DPE_WARNINGS "report_dpe_warnings"
#define CN10K_ML_DEV_CACHE_MODEL_DATA	"cache_model_data"
#define CN10K_ML_OCM_ALLOC_MODE		"ocm_alloc_mode"
#define CN10K_ML_DEV_HW_QUEUE_LOCK	"hw_queue_lock"
#define CN10K_ML_FW_POLL_MEM		"poll_mem"
#define CN10K_ML_OCM_PAGE_SIZE		"ocm_page_size"

#define CN10K_ML_FW_PATH_DEFAULT		"/lib/firmware/mlip-fw.bin"
#define CN10K_ML_FW_ENABLE_DPE_WARNINGS_DEFAULT 1
#define CN10K_ML_FW_REPORT_DPE_WARNINGS_DEFAULT 0
#define CN10K_ML_DEV_CACHE_MODEL_DATA_DEFAULT	1
#define CN10K_ML_OCM_ALLOC_MODE_DEFAULT		"lowest"
#define CN10K_ML_DEV_HW_QUEUE_LOCK_DEFAULT	1
#define CN10K_ML_FW_POLL_MEM_DEFAULT		"ddr"
#define CN10K_ML_OCM_PAGE_SIZE_DEFAULT		16384

/* ML firmware macros */
#define FW_MEMZONE_NAME		 "ml_cn10k_fw_mz"
#define FW_STACK_BUFFER_SIZE	 0x40000
#define FW_DEBUG_BUFFER_SIZE	 (2 * 0x20000)
#define FW_EXCEPTION_BUFFER_SIZE 0x400
#define FW_LINKER_OFFSET	 0x80000
#define FW_WAIT_CYCLES		 100

/* Firmware flags */
#define FW_ENABLE_DPE_WARNING_BITMASK BIT(0)
#define FW_REPORT_DPE_WARNING_BITMASK BIT(1)
#define FW_USE_DDR_POLL_ADDR_FP	      BIT(2)

static const char *const valid_args[] = {CN10K_ML_FW_PATH,
					 CN10K_ML_FW_ENABLE_DPE_WARNINGS,
					 CN10K_ML_FW_REPORT_DPE_WARNINGS,
					 CN10K_ML_DEV_CACHE_MODEL_DATA,
					 CN10K_ML_OCM_ALLOC_MODE,
					 CN10K_ML_DEV_HW_QUEUE_LOCK,
					 CN10K_ML_FW_POLL_MEM,
					 CN10K_ML_OCM_PAGE_SIZE,
					 NULL};

/* Supported OCM page sizes: 1KB, 2KB, 4KB, 8KB and 16KB */
static const int valid_ocm_page_size[] = {1024, 2048, 4096, 8192, 16384};

/* Dummy operations for ML device */
struct rte_ml_dev_ops ml_dev_dummy_ops = {0};

static int
parse_string_arg(const char *key __rte_unused, const char *value, void *extra_args)
{
	if (value == NULL || extra_args == NULL)
		return -EINVAL;

	*(char **)extra_args = strdup(value);

	if (!*(char **)extra_args)
		return -ENOMEM;

	return 0;
}

static int
parse_integer_arg(const char *key __rte_unused, const char *value, void *extra_args)
{
	int *i = (int *)extra_args;

	*i = atoi(value);
	if (*i < 0) {
		plt_err("Argument has to be positive.");
		return -EINVAL;
	}

	return 0;
}

static int
cn10k_mldev_parse_devargs(struct rte_devargs *devargs, struct cn10k_ml_dev *mldev)
{
	bool enable_dpe_warnings_set = false;
	bool report_dpe_warnings_set = false;
	bool cache_model_data_set = false;
	struct rte_kvargs *kvlist = NULL;
	bool ocm_alloc_mode_set = false;
	bool hw_queue_lock_set = false;
	bool ocm_page_size_set = false;
	char *ocm_alloc_mode = NULL;
	bool poll_mem_set = false;
	bool fw_path_set = false;
	char *poll_mem = NULL;
	char *fw_path = NULL;
	int ret = 0;
	bool found;
	uint8_t i;

	if (devargs == NULL)
		goto check_args;

	kvlist = rte_kvargs_parse(devargs->args, valid_args);
	if (kvlist == NULL) {
		plt_err("Error parsing devargs\n");
		return -EINVAL;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_FW_PATH) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_FW_PATH, &parse_string_arg, &fw_path);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n", CN10K_ML_FW_PATH);
			ret = -EINVAL;
			goto exit;
		}
		fw_path_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_FW_ENABLE_DPE_WARNINGS) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_FW_ENABLE_DPE_WARNINGS,
					 &parse_integer_arg, &mldev->fw.enable_dpe_warnings);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n",
				CN10K_ML_FW_ENABLE_DPE_WARNINGS);
			ret = -EINVAL;
			goto exit;
		}
		enable_dpe_warnings_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_FW_REPORT_DPE_WARNINGS) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_FW_REPORT_DPE_WARNINGS,
					 &parse_integer_arg, &mldev->fw.report_dpe_warnings);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n",
				CN10K_ML_FW_REPORT_DPE_WARNINGS);
			ret = -EINVAL;
			goto exit;
		}
		report_dpe_warnings_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_DEV_CACHE_MODEL_DATA) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_DEV_CACHE_MODEL_DATA, &parse_integer_arg,
					 &mldev->cache_model_data);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n",
				CN10K_ML_DEV_CACHE_MODEL_DATA);
			ret = -EINVAL;
			goto exit;
		}
		cache_model_data_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_OCM_ALLOC_MODE) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_OCM_ALLOC_MODE, &parse_string_arg,
					 &ocm_alloc_mode);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n", CN10K_ML_OCM_ALLOC_MODE);
			ret = -EINVAL;
			goto exit;
		}
		ocm_alloc_mode_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_DEV_HW_QUEUE_LOCK) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_DEV_HW_QUEUE_LOCK, &parse_integer_arg,
					 &mldev->hw_queue_lock);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n",
				CN10K_ML_DEV_HW_QUEUE_LOCK);
			ret = -EINVAL;
			goto exit;
		}
		hw_queue_lock_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_FW_POLL_MEM) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_FW_POLL_MEM, &parse_string_arg,
					 &poll_mem);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n", CN10K_ML_FW_POLL_MEM);
			ret = -EINVAL;
			goto exit;
		}
		poll_mem_set = true;
	}

	if (rte_kvargs_count(kvlist, CN10K_ML_OCM_PAGE_SIZE) == 1) {
		ret = rte_kvargs_process(kvlist, CN10K_ML_OCM_PAGE_SIZE, &parse_integer_arg,
					 &mldev->ocm_page_size);
		if (ret < 0) {
			plt_err("Error processing arguments, key = %s\n", CN10K_ML_OCM_PAGE_SIZE);
			ret = -EINVAL;
			goto exit;
		}
		ocm_page_size_set = true;
	}

check_args:
	if (!fw_path_set)
		mldev->fw.path = CN10K_ML_FW_PATH_DEFAULT;
	else
		mldev->fw.path = fw_path;
	plt_info("ML: %s = %s", CN10K_ML_FW_PATH, mldev->fw.path);

	if (!enable_dpe_warnings_set) {
		mldev->fw.enable_dpe_warnings = CN10K_ML_FW_ENABLE_DPE_WARNINGS_DEFAULT;
	} else {
		if ((mldev->fw.enable_dpe_warnings < 0) || (mldev->fw.enable_dpe_warnings > 1)) {
			plt_err("Invalid argument, %s = %d\n", CN10K_ML_FW_ENABLE_DPE_WARNINGS,
				mldev->fw.enable_dpe_warnings);
			ret = -EINVAL;
			goto exit;
		}
	}
	plt_info("ML: %s = %d", CN10K_ML_FW_ENABLE_DPE_WARNINGS, mldev->fw.enable_dpe_warnings);

	if (!report_dpe_warnings_set) {
		mldev->fw.report_dpe_warnings = CN10K_ML_FW_REPORT_DPE_WARNINGS_DEFAULT;
	} else {
		if ((mldev->fw.report_dpe_warnings < 0) || (mldev->fw.report_dpe_warnings > 1)) {
			plt_err("Invalid argument, %s = %d\n", CN10K_ML_FW_REPORT_DPE_WARNINGS,
				mldev->fw.report_dpe_warnings);
			ret = -EINVAL;
			goto exit;
		}
	}
	plt_info("ML: %s = %d", CN10K_ML_FW_REPORT_DPE_WARNINGS, mldev->fw.report_dpe_warnings);

	if (!cache_model_data_set) {
		mldev->cache_model_data = CN10K_ML_DEV_CACHE_MODEL_DATA_DEFAULT;
	} else {
		if ((mldev->cache_model_data < 0) || (mldev->cache_model_data > 1)) {
			plt_err("Invalid argument, %s = %d\n", CN10K_ML_DEV_CACHE_MODEL_DATA,
				mldev->cache_model_data);
			ret = -EINVAL;
			goto exit;
		}
	}
	plt_info("ML: %s = %d", CN10K_ML_DEV_CACHE_MODEL_DATA, mldev->cache_model_data);

	if (!ocm_alloc_mode_set) {
		mldev->ocm.alloc_mode = CN10K_ML_OCM_ALLOC_MODE_DEFAULT;
	} else {
		if (!((strcmp(ocm_alloc_mode, "lowest") == 0) ||
		      (strcmp(ocm_alloc_mode, "largest") == 0))) {
			plt_err("Invalid argument, %s = %s\n", CN10K_ML_OCM_ALLOC_MODE,
				ocm_alloc_mode);
			ret = -EINVAL;
			goto exit;
		}
		mldev->ocm.alloc_mode = ocm_alloc_mode;
	}
	plt_info("ML: %s = %s", CN10K_ML_OCM_ALLOC_MODE, mldev->ocm.alloc_mode);

	if (!hw_queue_lock_set) {
		mldev->hw_queue_lock = CN10K_ML_DEV_HW_QUEUE_LOCK_DEFAULT;
	} else {
		if ((mldev->hw_queue_lock < 0) || (mldev->hw_queue_lock > 1)) {
			plt_err("Invalid argument, %s = %d\n", CN10K_ML_DEV_HW_QUEUE_LOCK,
				mldev->hw_queue_lock);
			ret = -EINVAL;
			goto exit;
		}
	}
	plt_info("ML: %s = %d", CN10K_ML_DEV_HW_QUEUE_LOCK, mldev->hw_queue_lock);

	if (!poll_mem_set) {
		mldev->fw.poll_mem = CN10K_ML_FW_POLL_MEM_DEFAULT;
	} else {
		if (!((strcmp(poll_mem, "ddr") == 0) || (strcmp(poll_mem, "register") == 0))) {
			plt_err("Invalid argument, %s = %s\n", CN10K_ML_FW_POLL_MEM, poll_mem);
			ret = -EINVAL;
			goto exit;
		}
		mldev->fw.poll_mem = poll_mem;
	}
	plt_info("ML: %s = %s", CN10K_ML_FW_POLL_MEM, mldev->fw.poll_mem);

	if (!ocm_page_size_set) {
		mldev->ocm_page_size = CN10K_ML_OCM_PAGE_SIZE_DEFAULT;
	} else {
		if (mldev->ocm_page_size < 0) {
			plt_err("Invalid argument, %s = %d\n", CN10K_ML_OCM_PAGE_SIZE,
				mldev->ocm_page_size);
			ret = -EINVAL;
			goto exit;
		}

		found = false;
		for (i = 0; i < PLT_DIM(valid_ocm_page_size); i++) {
			if (mldev->ocm_page_size == valid_ocm_page_size[i]) {
				found = true;
				break;
			}
		}

		if (!found) {
			plt_err("Unsupported ocm_page_size = %d\n", mldev->ocm_page_size);
			ret = -EINVAL;
			goto exit;
		}
	}
	plt_info("ML: %s = %d", CN10K_ML_OCM_PAGE_SIZE, mldev->ocm_page_size);

exit:
	if (kvlist)
		rte_kvargs_free(kvlist);

	return ret;
}

static int
cn10k_ml_pci_probe(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	struct rte_ml_dev_pmd_init_params init_params;
	struct cn10k_ml_dev *mldev;
	char name[RTE_ML_STR_MAX];
	struct rte_ml_dev *dev;
	int ret;

	PLT_SET_USED(pci_drv);

	init_params = (struct rte_ml_dev_pmd_init_params){
		.socket_id = rte_socket_id(), .private_data_size = sizeof(struct cn10k_ml_dev)};

	ret = roc_plt_init();
	if (ret < 0) {
		plt_err("Failed to initialize platform model");
		return ret;
	}

	rte_pci_device_name(&pci_dev->addr, name, sizeof(name));
	dev = rte_ml_dev_pmd_create(name, &pci_dev->device, &init_params);
	if (dev == NULL) {
		ret = -ENODEV;
		goto error_exit;
	}

	/* Get private data space allocated */
	mldev = dev->data->dev_private;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		mldev->roc.pci_dev = pci_dev;

		ret = cn10k_mldev_parse_devargs(dev->device->devargs, mldev);
		if (ret) {
			plt_err("Failed to parse devargs ret = %d", ret);
			goto pmd_destroy;
		}

		ret = roc_ml_dev_init(&mldev->roc);
		if (ret) {
			plt_err("Failed to initialize ML ROC, ret = %d", ret);
			goto pmd_destroy;
		}

		dev->dev_ops = &cn10k_ml_ops;
	} else {
		plt_err("CN10K ML Ops are not supported on secondary process");
		dev->dev_ops = &ml_dev_dummy_ops;
	}

	dev->enqueue_burst = NULL;
	dev->dequeue_burst = NULL;
	dev->op_error_get = NULL;

	mldev->state = ML_CN10K_DEV_STATE_PROBED;

	return 0;

pmd_destroy:
	rte_ml_dev_pmd_destroy(dev);

error_exit:
	plt_err("Could not create device (vendor_id: 0x%x device_id: 0x%x)", pci_dev->id.vendor_id,
		pci_dev->id.device_id);

	return ret;
}

static int
cn10k_ml_pci_remove(struct rte_pci_device *pci_dev)
{
	struct cn10k_ml_dev *mldev;
	char name[RTE_ML_STR_MAX];
	struct rte_ml_dev *dev;
	int ret;

	if (pci_dev == NULL)
		return -EINVAL;

	rte_pci_device_name(&pci_dev->addr, name, sizeof(name));

	dev = rte_ml_dev_pmd_get_named_dev(name);
	if (dev == NULL)
		return -ENODEV;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		mldev = dev->data->dev_private;
		ret = roc_ml_dev_fini(&mldev->roc);
		if (ret)
			return ret;
	}

	return rte_ml_dev_pmd_destroy(dev);
}

static void
cn10k_ml_fw_print_info(struct cn10k_ml_fw *fw)
{
	plt_info("ML Firmware Version = %s", fw->req->jd.fw_load.version);

	plt_ml_dbg("Firmware capabilities = 0x%016lx", fw->req->jd.fw_load.cap.u64);
	plt_ml_dbg("Version = %s", fw->req->jd.fw_load.version);
	plt_ml_dbg("core0_debug_ptr = 0x%016lx", fw->req->jd.fw_load.debug.core0_debug_ptr);
	plt_ml_dbg("core1_debug_ptr = 0x%016lx", fw->req->jd.fw_load.debug.core1_debug_ptr);
	plt_ml_dbg("debug_buffer_size = %u bytes", fw->req->jd.fw_load.debug.debug_buffer_size);
	plt_ml_dbg("core0_exception_buffer = 0x%016lx",
		   fw->req->jd.fw_load.debug.core0_exception_buffer);
	plt_ml_dbg("core1_exception_buffer = 0x%016lx",
		   fw->req->jd.fw_load.debug.core1_exception_buffer);
	plt_ml_dbg("exception_state_size = %u bytes",
		   fw->req->jd.fw_load.debug.exception_state_size);
	plt_ml_dbg("flags = 0x%016lx", fw->req->jd.fw_load.flags);
}

uint64_t
cn10k_ml_fw_flags_get(struct cn10k_ml_fw *fw)
{
	uint64_t flags = 0x0;

	if (fw->enable_dpe_warnings)
		flags = flags | FW_ENABLE_DPE_WARNING_BITMASK;

	if (fw->report_dpe_warnings)
		flags = flags | FW_REPORT_DPE_WARNING_BITMASK;

	if (strcmp(fw->poll_mem, "ddr") == 0)
		flags = flags | FW_USE_DDR_POLL_ADDR_FP;
	else if (strcmp(fw->poll_mem, "register") == 0)
		flags = flags & ~FW_USE_DDR_POLL_ADDR_FP;

	return flags;
}

static int
cn10k_ml_fw_load_asim(struct cn10k_ml_fw *fw)
{
	struct cn10k_ml_dev *mldev;
	uint64_t timeout_cycle;
	uint64_t reg_val64;
	bool timeout;
	int ret = 0;

	mldev = fw->mldev;

	/* Reset HEAD and TAIL debug pointer registers */
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C1);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C1);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C1);

	/* Set ML_MLR_BASE to base IOVA of the ML region in LLC/DRAM. */
	reg_val64 = rte_eal_get_baseaddr();
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_MLR_BASE);
	plt_ml_dbg("ML_MLR_BASE = 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_MLR_BASE));
	roc_ml_reg_save(&mldev->roc, ML_MLR_BASE);

	/* Update FW load completion structure */
	fw->req->jd.hdr.jce.w1.u64 = PLT_U64_CAST(&fw->req->status);
	fw->req->jd.hdr.job_type = ML_CN10K_JOB_TYPE_FIRMWARE_LOAD;
	fw->req->jd.hdr.result = roc_ml_addr_ap2mlip(&mldev->roc, &fw->req->result);
	fw->req->jd.fw_load.flags = cn10k_ml_fw_flags_get(fw);
	plt_write64(ML_CN10K_POLL_JOB_START, &fw->req->status);
	plt_wmb();

	/* Enqueue FW load through scratch registers */
	timeout = true;
	timeout_cycle = plt_tsc_cycles() + ML_CN10K_CMD_TIMEOUT * plt_tsc_hz();
	roc_ml_scratch_enqueue(&mldev->roc, &fw->req->jd);

	plt_rmb();
	do {
		if (roc_ml_scratch_is_done_bit_set(&mldev->roc) &&
		    (plt_read64(&fw->req->status) == ML_CN10K_POLL_JOB_FINISH)) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() < timeout_cycle);

	/* Check firmware load status, clean-up and exit on failure. */
	if ((!timeout) && (fw->req->result.error_code.u64 == 0)) {
		cn10k_ml_fw_print_info(fw);
	} else {
		/* Set ML to disable new jobs */
		reg_val64 = (ROC_ML_CFG_JD_SIZE | ROC_ML_CFG_MLIP_ENA);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);

		/* Clear scratch registers */
		roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_WORK_PTR);
		roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_FW_CTRL);

		if (timeout) {
			plt_err("Firmware load timeout");
			ret = -ETIME;
		} else {
			plt_err("Firmware load failed");
			ret = -1;
		}

		return ret;
	}

	/* Reset scratch registers */
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_FW_CTRL);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_WORK_PTR);

	/* Disable job execution, to be enabled in start */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	return ret;
}

static int
cn10k_ml_fw_load_cn10ka(struct cn10k_ml_fw *fw, void *buffer, uint64_t size)
{
	union ml_a35_0_rst_vector_base_s a35_0_rst_vector_base;
	union ml_a35_0_rst_vector_base_s a35_1_rst_vector_base;
	struct cn10k_ml_dev *mldev;
	uint64_t timeout_cycle;
	uint64_t reg_val64;
	uint32_t reg_val32;
	uint64_t offset;
	bool timeout;
	int ret = 0;
	uint8_t i;

	mldev = fw->mldev;

	/* Reset HEAD and TAIL debug pointer registers */
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_HEAD_C1);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_DBG_BUFFER_TAIL_C1);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C0);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_EXCEPTION_SP_C1);

	/* (1) Write firmware images for ACC's two A35 cores to the ML region in LLC / DRAM. */
	rte_memcpy(PLT_PTR_ADD(fw->data, FW_LINKER_OFFSET), buffer, size);

	/* (2) Set ML(0)_MLR_BASE = Base IOVA of the ML region in LLC/DRAM. */
	reg_val64 = PLT_PTR_SUB_U64_CAST(fw->data, rte_eal_get_baseaddr());
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_MLR_BASE);
	plt_ml_dbg("ML_MLR_BASE => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_MLR_BASE));
	roc_ml_reg_save(&mldev->roc, ML_MLR_BASE);

	/* (3) Set ML(0)_AXI_BRIDGE_CTRL(1) = 0x184003 to remove back-pressure check on DMA AXI
	 * bridge.
	 */
	reg_val64 = (ROC_ML_AXI_BRIDGE_CTRL_AXI_RESP_CTRL |
		     ROC_ML_AXI_BRIDGE_CTRL_BRIDGE_CTRL_MODE | ROC_ML_AXI_BRIDGE_CTRL_NCB_WR_BLK |
		     ROC_ML_AXI_BRIDGE_CTRL_FORCE_WRESP_OK | ROC_ML_AXI_BRIDGE_CTRL_FORCE_RRESP_OK);
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_AXI_BRIDGE_CTRL(1));
	plt_ml_dbg("ML_AXI_BRIDGE_CTRL(1) => 0x%016lx",
		   roc_ml_reg_read64(&mldev->roc, ML_AXI_BRIDGE_CTRL(1)));

	/* (4) Set ML(0)_ANB(0..2)_BACKP_DISABLE = 0x3 to remove back-pressure on the AXI to NCB
	 * bridges.
	 */
	for (i = 0; i < ML_ANBX_NR; i++) {
		reg_val64 = (ROC_ML_ANBX_BACKP_DISABLE_EXTMSTR_B_BACKP_DISABLE |
			     ROC_ML_ANBX_BACKP_DISABLE_EXTMSTR_R_BACKP_DISABLE);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_ANBX_BACKP_DISABLE(i));
		plt_ml_dbg("ML_ANBX_BACKP_DISABLE(%u) => 0x%016lx", i,
			   roc_ml_reg_read64(&mldev->roc, ML_ANBX_BACKP_DISABLE(i)));
	}

	/* (5) Set ML(0)_ANB(0..2)_NCBI_P_OVR = 0x3000 and ML(0)_ANB(0..2)_NCBI_NP_OVR = 0x3000 to
	 * signal all ML transactions as non-secure.
	 */
	for (i = 0; i < ML_ANBX_NR; i++) {
		reg_val64 = (ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_NS_OVR |
			     ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_NS_OVR_VLD);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_ANBX_NCBI_P_OVR(i));
		plt_ml_dbg("ML_ANBX_NCBI_P_OVR(%u) => 0x%016lx", i,
			   roc_ml_reg_read64(&mldev->roc, ML_ANBX_NCBI_P_OVR(i)));

		reg_val64 |= (ML_ANBX_NCBI_NP_OVR_ANB_NCBI_NP_NS_OVR |
			      ML_ANBX_NCBI_NP_OVR_ANB_NCBI_NP_NS_OVR_VLD);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_ANBX_NCBI_NP_OVR(i));
		plt_ml_dbg("ML_ANBX_NCBI_NP_OVR(%u) => 0x%016lx", i,
			   roc_ml_reg_read64(&mldev->roc, ML_ANBX_NCBI_NP_OVR(i)));
	}

	/* (6) Set ML(0)_CFG[MLIP_CLK_FORCE] = 1, to force turning on the MLIP clock. */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_MLIP_CLK_FORCE;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	/* (7) Set ML(0)_JOB_MGR_CTRL[STALL_ON_IDLE] = 0, to make sure the boot request is accepted
	 * when there is no job in the command queue.
	 */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_JOB_MGR_CTRL);
	reg_val64 &= ~ROC_ML_JOB_MGR_CTRL_STALL_ON_IDLE;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_JOB_MGR_CTRL);
	plt_ml_dbg("ML_JOB_MGR_CTRL => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_JOB_MGR_CTRL));

	/* (8) Set ML(0)_CFG[ENA] = 0 and ML(0)_CFG[MLIP_ENA] = 1 to bring MLIP out of reset while
	 * keeping the job manager disabled.
	 */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_MLIP_ENA;
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	/* (9) Wait at least 70 coprocessor clock cycles. */
	plt_delay_us(FW_WAIT_CYCLES);

	/* (10) Write ML outbound addresses pointing to the firmware images written in step 1 to the
	 * following registers: ML(0)_A35_0_RST_VECTOR_BASE_W(0..1) for core 0,
	 * ML(0)_A35_1_RST_VECTOR_BASE_W(0..1) for core 1. The value written to each register is the
	 * AXI outbound address divided by 4. Read after write.
	 */
	offset = PLT_PTR_ADD_U64_CAST(
		fw->data, FW_LINKER_OFFSET - roc_ml_reg_read64(&mldev->roc, ML_MLR_BASE));
	a35_0_rst_vector_base.s.addr = (offset + ML_AXI_START_ADDR) / 4;
	a35_1_rst_vector_base.s.addr = (offset + ML_AXI_START_ADDR) / 4;

	roc_ml_reg_write32(&mldev->roc, a35_0_rst_vector_base.w.w0, ML_A35_0_RST_VECTOR_BASE_W(0));
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_A35_0_RST_VECTOR_BASE_W(0));
	plt_ml_dbg("ML_A35_0_RST_VECTOR_BASE_W(0) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&mldev->roc, a35_0_rst_vector_base.w.w1, ML_A35_0_RST_VECTOR_BASE_W(1));
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_A35_0_RST_VECTOR_BASE_W(1));
	plt_ml_dbg("ML_A35_0_RST_VECTOR_BASE_W(1) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&mldev->roc, a35_1_rst_vector_base.w.w0, ML_A35_1_RST_VECTOR_BASE_W(0));
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_A35_1_RST_VECTOR_BASE_W(0));
	plt_ml_dbg("ML_A35_1_RST_VECTOR_BASE_W(0) => 0x%08x", reg_val32);

	roc_ml_reg_write32(&mldev->roc, a35_1_rst_vector_base.w.w1, ML_A35_1_RST_VECTOR_BASE_W(1));
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_A35_1_RST_VECTOR_BASE_W(1));
	plt_ml_dbg("ML_A35_1_RST_VECTOR_BASE_W(1) => 0x%08x", reg_val32);

	/* (11) Clear MLIP's ML(0)_SW_RST_CTRL[ACC_RST]. This will bring the ACC cores and other
	 * MLIP components out of reset. The cores will execute firmware from the ML region as
	 * written in step 1.
	 */
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_SW_RST_CTRL);
	reg_val32 &= ~ROC_ML_SW_RST_CTRL_ACC_RST;
	roc_ml_reg_write32(&mldev->roc, reg_val32, ML_SW_RST_CTRL);
	reg_val32 = roc_ml_reg_read32(&mldev->roc, ML_SW_RST_CTRL);
	plt_ml_dbg("ML_SW_RST_CTRL => 0x%08x", reg_val32);

	/* (12) Wait for notification from firmware that ML is ready for job execution. */
	fw->req->jd.hdr.jce.w1.u64 = PLT_U64_CAST(&fw->req->status);
	fw->req->jd.hdr.job_type = ML_CN10K_JOB_TYPE_FIRMWARE_LOAD;
	fw->req->jd.hdr.result = roc_ml_addr_ap2mlip(&mldev->roc, &fw->req->result);
	fw->req->jd.fw_load.flags = cn10k_ml_fw_flags_get(fw);
	plt_write64(ML_CN10K_POLL_JOB_START, &fw->req->status);
	plt_wmb();

	/* Enqueue FW load through scratch registers */
	timeout = true;
	timeout_cycle = plt_tsc_cycles() + ML_CN10K_CMD_TIMEOUT * plt_tsc_hz();
	roc_ml_scratch_enqueue(&mldev->roc, &fw->req->jd);

	plt_rmb();
	do {
		if (roc_ml_scratch_is_done_bit_set(&mldev->roc) &&
		    (plt_read64(&fw->req->status) == ML_CN10K_POLL_JOB_FINISH)) {
			timeout = false;
			break;
		}
	} while (plt_tsc_cycles() < timeout_cycle);

	/* Check firmware load status, clean-up and exit on failure. */
	if ((!timeout) && (fw->req->result.error_code.u64 == 0)) {
		cn10k_ml_fw_print_info(fw);
	} else {
		/* Set ML to disable new jobs */
		reg_val64 = (ROC_ML_CFG_JD_SIZE | ROC_ML_CFG_MLIP_ENA);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);

		/* Clear scratch registers */
		roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_WORK_PTR);
		roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_FW_CTRL);

		if (timeout) {
			plt_err("Firmware load timeout");
			ret = -ETIME;
		} else {
			plt_err("Firmware load failed");
			ret = -1;
		}

		return ret;
	}

	/* (13) Set ML(0)_JOB_MGR_CTRL[STALL_ON_IDLE] = 0x1; this is needed to shut down the MLIP
	 * clock when there are no more jobs to process.
	 */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_JOB_MGR_CTRL);
	reg_val64 |= ROC_ML_JOB_MGR_CTRL_STALL_ON_IDLE;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_JOB_MGR_CTRL);
	plt_ml_dbg("ML_JOB_MGR_CTRL => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_JOB_MGR_CTRL));

	/* (14) Set ML(0)_CFG[MLIP_CLK_FORCE] = 0; the MLIP clock will be turned on/off based on job
	 * activities.
	 */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_MLIP_CLK_FORCE;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	/* (15) Set ML(0)_CFG[ENA] to enable ML job execution. */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 |= ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	/* Reset scratch registers */
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_FW_CTRL);
	roc_ml_reg_write64(&mldev->roc, 0, ML_SCRATCH_WORK_PTR);

	/* Disable job execution, to be enabled in start */
	reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val64 &= ~ROC_ML_CFG_ENA;
	roc_ml_reg_write64(&mldev->roc, reg_val64, ML_CFG);
	plt_ml_dbg("ML_CFG => 0x%016lx", roc_ml_reg_read64(&mldev->roc, ML_CFG));

	/* Additional fixes: Set RO bit to fix O2D DMA bandwidth issue on cn10ka */
	for (i = 0; i < ML_ANBX_NR; i++) {
		reg_val64 = roc_ml_reg_read64(&mldev->roc, ML_ANBX_NCBI_P_OVR(i));
		reg_val64 |= (ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_RO_OVR |
			      ML_ANBX_NCBI_P_OVR_ANB_NCBI_P_RO_OVR_VLD);
		roc_ml_reg_write64(&mldev->roc, reg_val64, ML_ANBX_NCBI_P_OVR(i));
	}

	return ret;
}

int
cn10k_ml_fw_load(struct cn10k_ml_dev *mldev)
{
	const struct plt_memzone *mz;
	struct cn10k_ml_fw *fw;
	void *fw_buffer = NULL;
	uint64_t mz_size = 0;
	uint64_t fw_size = 0;
	int ret = 0;

	fw = &mldev->fw;
	fw->mldev = mldev;

	if (roc_env_is_emulator() || roc_env_is_hw()) {
		/* Read firmware image to a buffer */
		ret = rte_firmware_read(fw->path, &fw_buffer, &fw_size);
		if (ret < 0) {
			plt_err("Can't read firmware data: %s\n", fw->path);
			return ret;
		}

		/* Reserve memzone for firmware load completion and data */
		mz_size = sizeof(struct cn10k_ml_req) + fw_size + FW_STACK_BUFFER_SIZE +
			  FW_DEBUG_BUFFER_SIZE + FW_EXCEPTION_BUFFER_SIZE;
	} else if (roc_env_is_asim()) {
		/* Reserve memzone for firmware load completion */
		mz_size = sizeof(struct cn10k_ml_req);
	}

	mz = plt_memzone_reserve_aligned(FW_MEMZONE_NAME, mz_size, 0, ML_CN10K_ALIGN_SIZE);
	if (mz == NULL) {
		plt_err("plt_memzone_reserve failed : %s", FW_MEMZONE_NAME);
		if (fw_buffer != NULL)
			free(fw_buffer);
		return -ENOMEM;
	}
	fw->req = mz->addr;

	/* Reset firmware load completion structure */
	memset(&fw->req->jd, 0, sizeof(struct cn10k_ml_jd));
	memset(&fw->req->jd.fw_load.version[0], '\0', MLDEV_FIRMWARE_VERSION_LENGTH);

	/* Reset device, if in active state */
	if (roc_ml_mlip_is_enabled(&mldev->roc))
		roc_ml_mlip_reset(&mldev->roc, true);

	/* Load firmware */
	if (roc_env_is_emulator() || roc_env_is_hw()) {
		fw->data = PLT_PTR_ADD(mz->addr, sizeof(struct cn10k_ml_req));
		ret = cn10k_ml_fw_load_cn10ka(fw, fw_buffer, fw_size);
		if (fw_buffer != NULL)
			free(fw_buffer);
	} else if (roc_env_is_asim()) {
		fw->data = NULL;
		ret = cn10k_ml_fw_load_asim(fw);
	}

	if (ret < 0)
		cn10k_ml_fw_unload(mldev);

	return ret;
}

void
cn10k_ml_fw_unload(struct cn10k_ml_dev *mldev)
{
	const struct plt_memzone *mz;
	uint64_t reg_val;

	/* Disable and reset device */
	reg_val = roc_ml_reg_read64(&mldev->roc, ML_CFG);
	reg_val &= ~ROC_ML_CFG_MLIP_ENA;
	roc_ml_reg_write64(&mldev->roc, reg_val, ML_CFG);
	roc_ml_mlip_reset(&mldev->roc, true);

	mz = plt_memzone_lookup(FW_MEMZONE_NAME);
	if (mz != NULL)
		plt_memzone_free(mz);
}

static struct rte_pci_id pci_id_ml_table[] = {
	{RTE_PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVID_CN10K_ML_PF)},
	/* sentinel */
	{},
};

static struct rte_pci_driver cn10k_mldev_pmd = {
	.id_table = pci_id_ml_table,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_NEED_IOVA_AS_VA,
	.probe = cn10k_ml_pci_probe,
	.remove = cn10k_ml_pci_remove,
};

RTE_PMD_REGISTER_PCI(MLDEV_NAME_CN10K_PMD, cn10k_mldev_pmd);
RTE_PMD_REGISTER_PCI_TABLE(MLDEV_NAME_CN10K_PMD, pci_id_ml_table);
RTE_PMD_REGISTER_KMOD_DEP(MLDEV_NAME_CN10K_PMD, "vfio-pci");

RTE_PMD_REGISTER_PARAM_STRING(MLDEV_NAME_CN10K_PMD, CN10K_ML_FW_PATH
			      "=<path>" CN10K_ML_FW_ENABLE_DPE_WARNINGS
			      "=<0|1>" CN10K_ML_FW_REPORT_DPE_WARNINGS
			      "=<0|1>" CN10K_ML_DEV_CACHE_MODEL_DATA
			      "=<0|1>" CN10K_ML_OCM_ALLOC_MODE
			      "=<lowest|largest>" CN10K_ML_DEV_HW_QUEUE_LOCK
			      "=<0|1>" CN10K_ML_FW_POLL_MEM "=<ddr|register>" CN10K_ML_OCM_PAGE_SIZE
			      "=<1024|2048|4096|8192|16384>");
