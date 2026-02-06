#include <asm/global_data.h>
#include <asm/io.h>
#include <clk.h>
#include <common.h>
#include <dm/device.h>
#include <dm/fdtaddr.h>
#include <dm/read.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/psci.h>
#include <log.h>
#include <watchdog.h>
#include <wdt.h>

#define WDT_SVC_ID              0x82003D06

enum smcwd_call {
	SMCWD_INIT		= 0,
	SMCWD_SET_TIMEOUT	= 1,
	SMCWD_ENABLE		= 2,
	SMCWD_PET		= 3,
	SMCWD_GET_TIMELEFT	= 4,
};

struct wdt_priv {
	unsigned long smc_func_id;
};

static int smcwd_call(struct udevice *dev, enum smcwd_call call, unsigned long arg, struct arm_smccc_res *res)
{
	struct arm_smccc_res local_res;
	struct wdt_priv *priv = dev_get_priv(dev);

	if (!res)
		res = &local_res;

	arm_smccc_smc(priv->smc_func_id, call, arg, 0, 0, 0, 0, 0, res);

	if (res->a0 == PSCI_RET_NOT_SUPPORTED)
		return -ENODEV;
	if (res->a0 == PSCI_RET_INVALID_PARAMS)
		return -EINVAL;
	if (res->a0 != PSCI_RET_SUCCESS)
		return -EIO;
	return 0;
}

int smc_wdt_reset(struct udevice *dev)
{
	return smcwd_call(dev, SMCWD_PET, 0, NULL);
}

int smc_wdt_start(struct udevice *dev, u64 timeout, ulong flags)
{
	u32 timeout_sec = (u32)(timeout / 1000);
	int err;

	err = smcwd_call(dev, SMCWD_SET_TIMEOUT, timeout_sec, NULL);
	if (err)
		return err;
	return smcwd_call(dev, SMCWD_ENABLE, 1, NULL);
}

int smc_wdt_stop(struct udevice *dev)
{
	return smcwd_call(dev, SMCWD_ENABLE, 0, NULL);
}

int smc_wdt_expire_now(struct udevice *dev, ulong flags)
{
	return smcwd_call(dev, SMCWD_SET_TIMEOUT, 0, NULL);
}

static int smc_wdt_probe(struct udevice *dev)
{
	debug("%s: Probing wdt%u (smc-wdt)\n", __func__, dev_seq(dev));
	return 0;
}

static int wdt_of_to_plat(struct udevice *dev)
{
	struct wdt_priv *priv = dev_get_priv(dev);

	priv->smc_func_id = dev_read_u32_default(dev, "arm,smc-id", WDT_SVC_ID);

	return 0;
}

static const struct wdt_ops smc_wdt_ops = {
	.start		= smc_wdt_start,
	.stop		= smc_wdt_stop,
	.reset		= smc_wdt_reset,
	.expire_now	= smc_wdt_expire_now,
};

static const struct udevice_id smc_wdt_ids[] = {
	{ .compatible = "arm,smc-wdt" },
	{}
};

U_BOOT_DRIVER(smc_wdt) = {
	.name		= "smc_wdt",
	.id		= UCLASS_WDT,
	.of_match	= smc_wdt_ids,
	.probe		= smc_wdt_probe,
	.priv_auto	= sizeof(struct wdt_priv),
	.of_to_plat	= wdt_of_to_plat,
	.ops		= &smc_wdt_ops,
};
