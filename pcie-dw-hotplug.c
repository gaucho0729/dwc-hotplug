// SPDX-License-Identifier: GPL-2.0
/*
 * DesignWare PCIe Root Complex glue driver with slot hot-plug support
 * Target: Linux 5.10 (drivers/pci/controller/dwc)
 *
 * Hot-plug model
 * --------------
 * Many DesignWare-based SoCs do not route the standard PCIe hot-plug
 * interrupt (Slot Status / pciehp), so native pciehp cannot be used.
 * This driver does hot-plug itself:
 *
 *   detect  : PRSNT# GPIO (optional) and/or SoC link-up/link-down IRQ
 *             (optional), else polling of the DWC link state.
 *   insert  : slot power on -> PERST# deassert -> LTSSM enable ->
 *             wait for link -> 100 ms -> pci_rescan_bus()
 *   removal : mark devices disconnected -> pci_stop_and_remove_bus_device()
 *             -> (PRSNT# gone) LTSSM disable, PERST# assert, power off
 *
 * The Root Port is marked is_hotplug_bridge so that the PCI core reserves
 * bus numbers and bridge windows (pci=hpmemsize=,hpiosize=,hpbussize=)
 * for devices that appear after boot, and native_pcie_hotplug is cleared
 * so pciehp does not also try to own the slot.
 *
 * SoC "app" (vendor glue) registers below are PLACEHOLDERS. Adjust the
 * APP_* definitions to your SoC's reference manual.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

#include "../../pci.h"
#include "pcie-designware.h"

/* ---- SoC-specific application registers (ADJUST FOR YOUR SoC) -------- */
#define APP_LTSSM_CTRL			0x000
#define  APP_LTSSM_EN			BIT(0)
#define APP_IRQ_STATUS			0x010	/* write-1-to-clear */
#define APP_IRQ_ENABLE			0x014
#define  APP_IRQ_LINK_UP		BIT(0)
#define  APP_IRQ_LINK_DOWN		BIT(1)
#define  APP_IRQ_HP_MASK		(APP_IRQ_LINK_UP | APP_IRQ_LINK_DOWN)
/* ---------------------------------------------------------------------- */

#define DW_HP_PERST_DELAY_MS		100	/* T_PVPERL (CEM r4.0, 2.9.2)  */
#define DW_HP_POST_LINK_DELAY_MS	100	/* PCIe r5.0, 6.6.1            */
#define DW_HP_LINK_TIMEOUT_MS		1000
#define DW_HP_PRSNT_DEBOUNCE_MS		100
#define DW_HP_LINK_DEBOUNCE_MS		20

static unsigned int poll_ms = 1000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Link poll interval when no hot-plug IRQ is available (0 = off)");

struct dw_hp_pcie {
	struct dw_pcie		*pci;
	void __iomem		*app_base;	/* optional */

	struct clk_bulk_data	*clks;
	int			num_clks;
	struct reset_control	*rst;		/* optional */
	struct regulator	*vpcie;		/* optional slot power */
	struct gpio_desc	*perst_gpio;	/* optional PERST# */
	struct gpio_desc	*prsnt_gpio;	/* optional PRSNT# */

	int			prsnt_irq;
	int			link_irq;

	struct mutex		lock;		/* serialises slot state */
	struct delayed_work	hp_work;
	bool			slot_on;
	bool			enumerated;
	bool			poll;
};

#define to_dw_hp(x)	dev_get_drvdata((x)->dev)

static struct platform_driver dw_hp_pcie_driver;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static void dw_hp_app_rmw(struct dw_hp_pcie *hp, u32 reg, u32 clr, u32 set)
{
	u32 val;

	if (!hp->app_base)
		return;
	val = readl(hp->app_base + reg);
	val = (val & ~clr) | set;
	writel(val, hp->app_base + reg);
}

static int dw_hp_start_link(struct dw_pcie *pci)
{
	dw_hp_app_rmw(to_dw_hp(pci), APP_LTSSM_CTRL, 0, APP_LTSSM_EN);
	return 0;
}

static void dw_hp_stop_link(struct dw_pcie *pci)
{
	dw_hp_app_rmw(to_dw_hp(pci), APP_LTSSM_CTRL, APP_LTSSM_EN, 0);
}

static bool dw_hp_card_present(struct dw_hp_pcie *hp)
{
	/*
	 * Without PRSNT# we cannot tell; keep the slot powered and rely on
	 * the LTSSM's receiver detection.
	 */
	if (!hp->prsnt_gpio)
		return true;
	return gpiod_get_value_cansleep(hp->prsnt_gpio) > 0;
}

static int dw_hp_wait_link(struct dw_hp_pcie *hp, unsigned int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	do {
		if (dw_pcie_link_up(hp->pci))
			return 0;
		usleep_range(10000, 11000);
	} while (time_before(jiffies, end));

	return dw_pcie_link_up(hp->pci) ? 0 : -ETIMEDOUT;
}

static int dw_hp_slot_power_on(struct dw_hp_pcie *hp)
{
	struct device *dev = hp->pci->dev;
	int ret;

	if (hp->slot_on)
		return 0;

	gpiod_set_value_cansleep(hp->perst_gpio, 1);	/* assert PERST# */

	if (hp->vpcie) {
		ret = regulator_enable(hp->vpcie);
		if (ret) {
			dev_err(dev, "slot power on failed: %d\n", ret);
			return ret;
		}
	}

	msleep(DW_HP_PERST_DELAY_MS);
	gpiod_set_value_cansleep(hp->perst_gpio, 0);	/* deassert PERST# */

	dw_hp_start_link(hp->pci);
	hp->slot_on = true;
	return 0;
}

static void dw_hp_slot_power_off(struct dw_hp_pcie *hp)
{
	if (!hp->slot_on)
		return;

	dw_hp_stop_link(hp->pci);
	gpiod_set_value_cansleep(hp->perst_gpio, 1);
	if (hp->vpcie)
		regulator_disable(hp->vpcie);
	hp->slot_on = false;
}

/* ------------------------------------------------------------------ */
/* Enumeration / removal                                               */
/* ------------------------------------------------------------------ */

static void dw_hp_rescan(struct dw_hp_pcie *hp)
{
	pci_lock_rescan_remove();
	pci_rescan_bus(hp->pci->pp.bridge->bus);
	pci_unlock_rescan_remove();
}

static void dw_hp_remove_devices(struct dw_hp_pcie *hp, bool surprise)
{
	struct pci_bus *root = hp->pci->pp.bridge->bus;
	struct pci_dev *rp, *dev, *tmp;
	struct pci_bus *sub;

	pci_lock_rescan_remove();

	list_for_each_entry(rp, &root->devices, bus_list) {
		sub = rp->subordinate;
		if (!sub)
			continue;

		/*
		 * Fail further accesses fast; drivers check this in their
		 * error paths, and it avoids MMIO waits on a dead link.
		 */
		if (surprise)
			pci_walk_bus(sub, pci_dev_set_disconnected, NULL);

		list_for_each_entry_safe_reverse(dev, tmp, &sub->devices,
						 bus_list) {
			pci_dev_get(dev);
			pci_stop_and_remove_bus_device(dev);
			pci_dev_put(dev);
		}
	}

	pci_unlock_rescan_remove();
}

static void dw_hp_work(struct work_struct *work)
{
	struct dw_hp_pcie *hp = container_of(to_delayed_work(work),
					     struct dw_hp_pcie, hp_work);
	struct device *dev = hp->pci->dev;
	bool present, link;

	mutex_lock(&hp->lock);

	present = dw_hp_card_present(hp);
	link = present && hp->slot_on && dw_pcie_link_up(hp->pci);

	/* Removal (card pulled, or link lost) */
	if (hp->enumerated && !link) {
		dev_info(dev, "slot: %s, removing devices\n",
			 present ? "link down" : "card removed");
		dw_hp_remove_devices(hp, true);
		hp->enumerated = false;
	}

	if (!present) {
		dw_hp_slot_power_off(hp);
		goto out;
	}

	/* Insertion */
	if (!hp->enumerated) {
		if (!hp->slot_on) {
			if (dw_hp_slot_power_on(hp))
				goto out;
			/* PRSNT# just asserted: give the link time to train */
			if (dw_hp_wait_link(hp, DW_HP_LINK_TIMEOUT_MS)) {
				dev_warn(dev, "card present but link did not come up\n");
				goto out;
			}
		} else if (!dw_pcie_link_up(hp->pci)) {
			goto out;	/* nothing attached yet */
		}

		dev_info(dev, "slot: link up, enumerating\n");
		msleep(DW_HP_POST_LINK_DELAY_MS);
		dw_hp_rescan(hp);
		hp->enumerated = true;
	}

out:
	if (hp->poll && poll_ms)
		queue_delayed_work(system_long_wq, &hp->hp_work,
				   msecs_to_jiffies(poll_ms));
	mutex_unlock(&hp->lock);
}

static void dw_hp_kick(struct dw_hp_pcie *hp, unsigned int delay_ms)
{
	mod_delayed_work(system_long_wq, &hp->hp_work,
			 msecs_to_jiffies(delay_ms));
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static irqreturn_t dw_hp_prsnt_irq(int irq, void *data)
{
	dw_hp_kick(data, DW_HP_PRSNT_DEBOUNCE_MS);
	return IRQ_HANDLED;
}

static irqreturn_t dw_hp_link_irq(int irq, void *data)
{
	struct dw_hp_pcie *hp = data;
	u32 st;

	st = readl(hp->app_base + APP_IRQ_STATUS) & APP_IRQ_HP_MASK;
	if (!st)
		return IRQ_NONE;

	writel(st, hp->app_base + APP_IRQ_STATUS);	/* W1C */
	dw_hp_kick(hp, DW_HP_LINK_DEBOUNCE_MS);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Root Port fixup: reserve resources for hot-plug                     */
/* ------------------------------------------------------------------ */

static void dw_hp_fixup_root_port(struct pci_dev *pdev)
{
	struct pci_host_bridge *bridge;
	struct device *parent;

	if (!pci_is_root_bus(pdev->bus) ||
	    pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT)
		return;

	bridge = pci_find_host_bridge(pdev->bus);
	parent = bridge->dev.parent;
	if (!parent || parent->driver != &dw_hp_pcie_driver.driver)
		return;

	pdev->is_hotplug_bridge = 1;
	pci_info(pdev, "marked as hot-plug bridge\n");
}
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS_BRIDGE_PCI, 8,
			      dw_hp_fixup_root_port);

/* ------------------------------------------------------------------ */
/* DWC host ops                                                        */
/* ------------------------------------------------------------------ */

static int dw_hp_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct dw_hp_pcie *hp = to_dw_hp(pci);

	/* We own the slot; keep pciehp away from the Root Port. */
	pp->bridge->native_pcie_hotplug = 0;

	dw_pcie_setup_rc(pp);

	if (dw_hp_card_present(hp) && !dw_hp_slot_power_on(hp)) {
		if (!dw_pcie_wait_for_link(pci))
			hp->enumerated = true;	/* pci_host_probe() will scan */
	}

	dw_pcie_msi_init(pp);
	return 0;
}

static const struct dw_pcie_host_ops dw_hp_host_ops = {
	.host_init = dw_hp_host_init,
};

static const struct dw_pcie_ops dw_hp_pcie_ops = {
	.start_link = dw_hp_start_link,
	.stop_link = dw_hp_stop_link,
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static int dw_hp_get_resources(struct platform_device *pdev,
			       struct dw_hp_pcie *hp)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = hp->pci;
	struct resource *res;

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "app");
	if (res) {
		hp->app_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(hp->app_base))
			return PTR_ERR(hp->app_base);
	}

	hp->num_clks = devm_clk_bulk_get_all(dev, &hp->clks);
	if (hp->num_clks < 0)
		return dev_err_probe(dev, hp->num_clks, "failed to get clocks\n");

	hp->rst = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(hp->rst))
		return dev_err_probe(dev, PTR_ERR(hp->rst), "failed to get resets\n");

	hp->vpcie = devm_regulator_get_optional(dev, "vpcie");
	if (IS_ERR(hp->vpcie)) {
		if (PTR_ERR(hp->vpcie) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(hp->vpcie),
					     "failed to get vpcie supply\n");
		hp->vpcie = NULL;
	}

	/* "reset-gpios" is PERST#; describe it GPIO_ACTIVE_LOW in DT. */
	hp->perst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hp->perst_gpio))
		return dev_err_probe(dev, PTR_ERR(hp->perst_gpio),
				     "failed to get reset-gpios\n");

	/* "prsnt-gpios": logical 1 = card present (use GPIO_ACTIVE_LOW for PRSNT#) */
	hp->prsnt_gpio = devm_gpiod_get_optional(dev, "prsnt", GPIOD_IN);
	if (IS_ERR(hp->prsnt_gpio))
		return dev_err_probe(dev, PTR_ERR(hp->prsnt_gpio),
				     "failed to get prsnt-gpios\n");

	if (pci_msi_enabled()) {
		pci->pp.msi_irq = platform_get_irq_byname_optional(pdev, "msi");
		if (pci->pp.msi_irq < 0)
			pci->pp.msi_irq = 0;
	}

	hp->link_irq = hp->app_base ?
		platform_get_irq_byname_optional(pdev, "hp") : -ENXIO;

	return 0;
}

static int dw_hp_setup_irqs(struct dw_hp_pcie *hp)
{
	struct device *dev = hp->pci->dev;
	int ret;

	if (hp->prsnt_gpio) {
		hp->prsnt_irq = gpiod_to_irq(hp->prsnt_gpio);
		if (hp->prsnt_irq >= 0) {
			ret = devm_request_irq(dev, hp->prsnt_irq,
					       dw_hp_prsnt_irq,
					       IRQF_TRIGGER_RISING |
					       IRQF_TRIGGER_FALLING,
					       "pcie-prsnt", hp);
			if (ret)
				return ret;
		}
	}

	if (hp->link_irq > 0) {
		writel(APP_IRQ_HP_MASK, hp->app_base + APP_IRQ_STATUS);
		ret = devm_request_irq(dev, hp->link_irq, dw_hp_link_irq,
				       IRQF_SHARED, "pcie-hp", hp);
		if (ret)
			return ret;
		dw_hp_app_rmw(hp, APP_IRQ_ENABLE, 0, APP_IRQ_HP_MASK);
	}

	/* Fall back to polling when nothing tells us about link changes. */
	hp->poll = hp->link_irq <= 0 &&
		   (!hp->prsnt_gpio || hp->prsnt_irq < 0);
	if (hp->poll)
		dev_info(dev, "no hot-plug IRQ, polling every %u ms\n", poll_ms);

	return 0;
}

static int dw_hp_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_hp_pcie *hp;
	struct dw_pcie *pci;
	int ret;

	hp = devm_kzalloc(dev, sizeof(*hp), GFP_KERNEL);
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!hp || !pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_hp_pcie_ops;
	pci->pp.ops = &dw_hp_host_ops;
	hp->pci = pci;
	hp->prsnt_irq = -ENXIO;
	mutex_init(&hp->lock);
	INIT_DELAYED_WORK(&hp->hp_work, dw_hp_work);
	platform_set_drvdata(pdev, hp);

	ret = dw_hp_get_resources(pdev, hp);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(hp->num_clks, hp->clks);
	if (ret)
		return ret;

	ret = reset_control_deassert(hp->rst);
	if (ret)
		goto err_clk;

	ret = dw_pcie_host_init(&pci->pp);
	if (ret) {
		dev_err(dev, "failed to initialize host: %d\n", ret);
		goto err_rst;
	}

	ret = dw_hp_setup_irqs(hp);
	if (ret) {
		dev_err(dev, "failed to request hot-plug IRQs: %d\n", ret);
		goto err_host;
	}

	/* Sync state with reality (card may have changed during probe). */
	dw_hp_kick(hp, 0);
	return 0;

err_host:
	dw_pcie_host_deinit(&pci->pp);
err_rst:
	mutex_lock(&hp->lock);
	dw_hp_slot_power_off(hp);
	mutex_unlock(&hp->lock);
	reset_control_assert(hp->rst);
err_clk:
	clk_bulk_disable_unprepare(hp->num_clks, hp->clks);
	return ret;
}

static void dw_hp_pcie_shutdown(struct platform_device *pdev)
{
	struct dw_hp_pcie *hp = platform_get_drvdata(pdev);

	mutex_lock(&hp->lock);
	hp->poll = false;
	mutex_unlock(&hp->lock);
	cancel_delayed_work_sync(&hp->hp_work);
	if (hp->link_irq > 0)
		dw_hp_app_rmw(hp, APP_IRQ_ENABLE, APP_IRQ_HP_MASK, 0);
}

static const struct of_device_id dw_hp_pcie_of_match[] = {
	{ .compatible = "vendor,dw-pcie-hp" },	/* CHANGE to your SoC */
	{ }
};

static struct platform_driver dw_hp_pcie_driver = {
	.probe = dw_hp_pcie_probe,
	.shutdown = dw_hp_pcie_shutdown,
	.driver = {
		.name = "dw-pcie-hp",
		.of_match_table = dw_hp_pcie_of_match,
		/* PCI fixups only run for built-in code; no unbind either. */
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(dw_hp_pcie_driver);
