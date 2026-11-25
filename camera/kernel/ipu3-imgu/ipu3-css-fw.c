// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ipu3-css.h"
#include "ipu3-css-fw.h"
#include "ipu3-dmamap.h"

/*
 * The CF-20's working Windows camera package contains a second IPU3 CSS
 * image.  Keep it opt-in at the module level and only consider it when the
 * complete CF-20 camera ACPI set is present.  The normal Debian firmware
 * remains the fallback for every other machine or a missing local image.
 */
static bool cf20_windows_firmware;
module_param_named(cf20_windows_firmware, cf20_windows_firmware, bool, 0644);
MODULE_PARM_DESC(cf20_windows_firmware,
		 "Use the locally supplied CF-20 Windows CSS firmware when available");

/*
 * The CF-20 Windows CSS image has been proven to enumerate and to load, but
 * its Linux IMGU streaming ABI has not been proven safe on this machine.  A
 * failed CSS start can wedge the IMGU hard enough to freeze the whole
 * desktop. Keep the risky transition opt-in until the ABI is validated.
 * CIO2 raw capture is independent of this switch.
 */
static bool cf20_css_streaming;
module_param_named(cf20_css_streaming, cf20_css_streaming, bool, 0644);
MODULE_PARM_DESC(cf20_css_streaming,
		 "Allow CF-20 IMGU/CSS streaming (unsafe until validated)");

/*
 * The upstream IPU3 heuristic uses 200 MHz for the CF-20's 1280x720
 * viewfinder request.  The supplied Windows host exposes an explicit
 * ManualISPFreqControl/PowerMode path, so keep a bounded diagnostic override
 * available without changing the default for any machine.  Only the two
 * frequencies supported by the IPU3 wrapper are accepted by the caller.
 */
static unsigned int cf20_isp_freq;
module_param_named(cf20_isp_freq, cf20_isp_freq, uint, 0644);
MODULE_PARM_DESC(cf20_isp_freq,
		 "CF-20 IPU3 CSS frequency override in MHz (0=automatic, 200, or 450)");

static bool imgu_css_cf20_platform(void)
{
	/*
	 * The working rear camera is INT3477 and its TPS68470 provider is
	 * INT3472.  INT3471 belongs to the older IMX135 experiment and is not
	 * a prerequisite for the OV8858 path.  Requiring it would silently
	 * disable this safety gate on an OV8858-only enumeration.
	 */
	return acpi_dev_found("INT3472") &&
	       acpi_dev_found("INT3477");
}

bool imgu_css_cf20_streaming_allowed(void)
{
	/*
	 * The Windows CSS image is useful for offline identification and
	 * firmware-loading diagnostics only.  It was built for the Windows
	 * camera host/CPF path, not for the Linux IPU3 CSS ABI.  The Debian
	 * image also wedges this CF-20's IMGU during bootloader start, even with
	 * the measured 200 MHz and 450 MHz clock choices.  A failed start has
	 * already been observed to freeze the complete desktop, so the safety
	 * decision must not be overridable by a modprobe parameter.  Keep the
	 * parameter for compatibility with existing configuration files and for
	 * future hardware validation, but never permit live CF-20 CSS streaming
	 * until the host/firmware ABI has been matched and verified.
	 */
	if (imgu_css_cf20_platform())
		return false;

	return true;
}

unsigned int imgu_css_cf20_isp_freq(void)
{
	if (!imgu_css_cf20_platform())
		return 0;

	return cf20_isp_freq;
}

static void imgu_css_fw_show_binary(struct device *dev, struct imgu_fw_info *bi,
				    const char *name)
{
	unsigned int i;

	dev_dbg(dev, "found firmware binary type %i size %i name %s\n",
		bi->type, bi->blob.size, name);
	if (bi->type != IMGU_FW_ISP_FIRMWARE)
		return;

	dev_dbg(dev, "    id %i mode %i bds 0x%x veceven %i/%i out_pins %i\n",
		bi->info.isp.sp.id, bi->info.isp.sp.pipeline.mode,
		bi->info.isp.sp.bds.supported_bds_factors,
		bi->info.isp.sp.enable.vf_veceven,
		bi->info.isp.sp.vf_dec.is_variable,
		bi->info.isp.num_output_pins);

	dev_dbg(dev, "    input (%i,%i)-(%i,%i) formats %s%s%s\n",
		bi->info.isp.sp.input.min_width,
		bi->info.isp.sp.input.min_height,
		bi->info.isp.sp.input.max_width,
		bi->info.isp.sp.input.max_height,
		bi->info.isp.sp.enable.input_yuv ? "yuv420 " : "",
		bi->info.isp.sp.enable.input_feeder ||
		bi->info.isp.sp.enable.input_raw ? "raw8 raw10 " : "",
		bi->info.isp.sp.enable.input_raw ? "raw12" : "");

	dev_dbg(dev, "    internal (%i,%i)\n",
		bi->info.isp.sp.internal.max_width,
		bi->info.isp.sp.internal.max_height);

	dev_dbg(dev, "    output (%i,%i)-(%i,%i) formats",
		bi->info.isp.sp.output.min_width,
		bi->info.isp.sp.output.min_height,
		bi->info.isp.sp.output.max_width,
		bi->info.isp.sp.output.max_height);
	for (i = 0; i < bi->info.isp.num_output_formats; i++)
		dev_dbg(dev, " %i", bi->info.isp.output_formats[i]);
	dev_dbg(dev, " vf");
	for (i = 0; i < bi->info.isp.num_vf_formats; i++)
		dev_dbg(dev, " %i", bi->info.isp.vf_formats[i]);
	dev_dbg(dev, "\n");
}

unsigned int imgu_css_fw_obgrid_size(const struct imgu_fw_info *bi)
{
	unsigned int width = DIV_ROUND_UP(bi->info.isp.sp.internal.max_width,
					  IMGU_OBGRID_TILE_SIZE * 2) + 1;
	unsigned int height = DIV_ROUND_UP(bi->info.isp.sp.internal.max_height,
					   IMGU_OBGRID_TILE_SIZE * 2) + 1;
	unsigned int obgrid_size;

	width = ALIGN(width, IPU3_UAPI_ISP_VEC_ELEMS / 4);
	obgrid_size = PAGE_ALIGN(width * height *
				 sizeof(struct ipu3_uapi_obgrid_param)) *
				 bi->info.isp.sp.iterator.num_stripes;
	return obgrid_size;
}

void *imgu_css_fw_pipeline_params(struct imgu_css *css, unsigned int pipe,
				  enum imgu_abi_param_class cls,
				  enum imgu_abi_memories mem,
				  struct imgu_fw_isp_parameter *par,
				  size_t par_size, void *binary_params)
{
	struct imgu_fw_info *bi =
		&css->fwp->binary_header[css->pipes[pipe].bindex];

	if (par->offset + par->size >
	    bi->info.isp.sp.mem_initializers.params[cls][mem].size)
		return NULL;

	if (par->size != par_size)
		pr_warn("parameter size doesn't match defined size\n");

	if (par->size < par_size)
		return NULL;

	return binary_params + par->offset;
}

void imgu_css_fw_cleanup(struct imgu_css *css)
{
	struct imgu_device *imgu = dev_get_drvdata(css->dev);

	if (css->binary) {
		unsigned int i;

		for (i = 0; i < css->fwp->file_header.binary_nr; i++)
			imgu_dmamap_free(imgu, &css->binary[i]);
		kfree(css->binary);
	}
	if (css->fw)
		release_firmware(css->fw);

	css->binary = NULL;
	css->fw = NULL;
}

int imgu_css_fw_init(struct imgu_css *css)
{
	static const u32 BLOCK_MAX = 65536;
	struct imgu_device *imgu = dev_get_drvdata(css->dev);
	struct device *dev = css->dev;
	unsigned int i, j, binary_nr;
	int r;

	/*
	 * The supplied Windows image belongs to the Windows iacamera/iaisp
	 * host stack.  It is accompanied there by the Windows CPF/pipeCfg
	 * selection and is not a drop-in replacement for the Linux CSS ABI.
	 * Loading it on the CF-20 has already wedged the IMGU hard enough to
	 * freeze the desktop, sometimes before userspace starts streaming.
	 * Keep the module parameter for backwards-compatible diagnostics, but
	 * refuse the image on the affected platform.  This makes an old
	 * modprobe.d override fail safe after a reboot instead of reintroducing
	 * the known-dangerous firmware path.
	 */
	if (cf20_windows_firmware && imgu_css_cf20_platform())
		dev_warn(dev,
			 "CF-20 Windows CSS firmware rejected: Windows host/CPF ABI is not Linux-safe; using Debian firmware\n");

	if (!css->fw)
		r = request_firmware(&css->fw, IMGU_FW_NAME_IPU_20161208, css->dev);
	if (r == -ENOENT)
		r = request_firmware(&css->fw, IMGU_FW_NAME_20161208, css->dev);
	if (r == -ENOENT)
		r = request_firmware(&css->fw, IMGU_FW_NAME, css->dev);
	if (r)
		return r;

	/* Check and display fw header info */

	css->fwp = (struct imgu_fw_header *)css->fw->data;
	if (css->fw->size < struct_size(css->fwp, binary_header, 1) ||
	    css->fwp->file_header.h_size != sizeof(struct imgu_fw_bi_file_h))
		goto bad_fw;
	if (struct_size(css->fwp, binary_header,
			css->fwp->file_header.binary_nr) > css->fw->size)
		goto bad_fw;

	dev_info(dev, "loaded firmware version %.64s, %u binaries, %zu bytes\n",
		 css->fwp->file_header.version, css->fwp->file_header.binary_nr,
		 css->fw->size);

	/* Validate and display info on fw binaries */

	binary_nr = css->fwp->file_header.binary_nr;

	css->fw_bl = -1;
	css->fw_sp[0] = -1;
	css->fw_sp[1] = -1;

	for (i = 0; i < binary_nr; i++) {
		struct imgu_fw_info *bi = &css->fwp->binary_header[i];
		const char *name = (void *)css->fwp + bi->blob.prog_name_offset;
		size_t len;

		if (bi->blob.prog_name_offset >= css->fw->size)
			goto bad_fw;
		len = strnlen(name, css->fw->size - bi->blob.prog_name_offset);
		if (len + 1 > css->fw->size - bi->blob.prog_name_offset ||
		    len + 1 >= IMGU_ABI_MAX_BINARY_NAME)
			goto bad_fw;

		if (bi->blob.size != bi->blob.text_size + bi->blob.icache_size
		    + bi->blob.data_size + bi->blob.padding_size)
			goto bad_fw;
		if (bi->blob.offset + bi->blob.size > css->fw->size)
			goto bad_fw;

		if (bi->type == IMGU_FW_BOOTLOADER_FIRMWARE) {
			css->fw_bl = i;
			if (bi->info.bl.sw_state >= css->iomem_length ||
			    bi->info.bl.num_dma_cmds >= css->iomem_length ||
			    bi->info.bl.dma_cmd_list >= css->iomem_length)
				goto bad_fw;
		}
		if (bi->type == IMGU_FW_SP_FIRMWARE ||
		    bi->type == IMGU_FW_SP1_FIRMWARE) {
			css->fw_sp[bi->type == IMGU_FW_SP_FIRMWARE ? 0 : 1] = i;
			if (bi->info.sp.per_frame_data >= css->iomem_length ||
			    bi->info.sp.init_dmem_data >= css->iomem_length ||
			    bi->info.sp.host_sp_queue >= css->iomem_length ||
			    bi->info.sp.isp_started >= css->iomem_length ||
			    bi->info.sp.sw_state >= css->iomem_length ||
			    bi->info.sp.sleep_mode >= css->iomem_length ||
			    bi->info.sp.invalidate_tlb >= css->iomem_length ||
			    bi->info.sp.host_sp_com >= css->iomem_length ||
			    bi->info.sp.output + 12 >= css->iomem_length ||
			    bi->info.sp.host_sp_queues_initialized >=
			    css->iomem_length)
				goto bad_fw;
		}
		if (bi->type != IMGU_FW_ISP_FIRMWARE)
			continue;

		if (bi->info.isp.sp.pipeline.mode >= IPU3_CSS_PIPE_ID_NUM)
			goto bad_fw;

		if (bi->info.isp.sp.iterator.num_stripes >
		    IPU3_UAPI_MAX_STRIPES)
			goto bad_fw;

		if (bi->info.isp.num_vf_formats > IMGU_ABI_FRAME_FORMAT_NUM ||
		    bi->info.isp.num_output_formats > IMGU_ABI_FRAME_FORMAT_NUM)
			goto bad_fw;

		for (j = 0; j < bi->info.isp.num_output_formats; j++)
			if (bi->info.isp.output_formats[j] >=
			    IMGU_ABI_FRAME_FORMAT_NUM)
				goto bad_fw;
		for (j = 0; j < bi->info.isp.num_vf_formats; j++)
			if (bi->info.isp.vf_formats[j] >=
			    IMGU_ABI_FRAME_FORMAT_NUM)
				goto bad_fw;

		if (bi->info.isp.sp.block.block_width <= 0 ||
		    bi->info.isp.sp.block.block_width > BLOCK_MAX ||
		    bi->info.isp.sp.block.output_block_height <= 0 ||
		    bi->info.isp.sp.block.output_block_height > BLOCK_MAX)
			goto bad_fw;

		if (bi->blob.memory_offsets.offsets[IMGU_ABI_PARAM_CLASS_PARAM]
		    + sizeof(struct imgu_fw_param_memory_offsets) >
		    css->fw->size ||
		    bi->blob.memory_offsets.offsets[IMGU_ABI_PARAM_CLASS_CONFIG]
		    + sizeof(struct imgu_fw_config_memory_offsets) >
		    css->fw->size ||
		    bi->blob.memory_offsets.offsets[IMGU_ABI_PARAM_CLASS_STATE]
		    + sizeof(struct imgu_fw_state_memory_offsets) >
		    css->fw->size)
			goto bad_fw;

		imgu_css_fw_show_binary(dev, bi, name);
	}

	if (css->fw_bl == -1 || css->fw_sp[0] == -1 || css->fw_sp[1] == -1)
		goto bad_fw;

	/* Allocate and map fw binaries into IMGU */

	css->binary = kzalloc_objs(*css->binary, binary_nr);
	if (!css->binary) {
		r = -ENOMEM;
		goto error_out;
	}

	for (i = 0; i < css->fwp->file_header.binary_nr; i++) {
		struct imgu_fw_info *bi = &css->fwp->binary_header[i];
		void *blob = (void *)css->fwp + bi->blob.offset;
		size_t size = bi->blob.size;

		if (!imgu_dmamap_alloc(imgu, &css->binary[i], size)) {
			r = -ENOMEM;
			goto error_out;
		}
		memcpy(css->binary[i].vaddr, blob, size);
	}

	return 0;

bad_fw:
	dev_err(dev, "invalid firmware binary, size %u\n", (int)css->fw->size);
	r = -ENODEV;

error_out:
	imgu_css_fw_cleanup(css);
	return r;
}
