// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic XBurst SoCs SYSOST clocks driver
 * Copyright (c) 2020 周琰杰 (Zhou Yanjie) <zhouyanjie@wanyeetech.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>

#include <dt-bindings/clock/ingenic,sysost.h>

/* OST register offsets */
#define OST_REG_OSTCCR			0x00
#define OST_REG_OSTER			0x04
#define OST_REG_OSTCR			0x08
#define OST_REG_OSTFR			0x0c
#define OST_REG_OSTCNTH			0x0c
#define OST_REG_OSTMR			0x10
#define OST_REG_OSTCNTL			0x10
#define OST_REG_OST1DFR			0x14
#define OST_REG_OSTCNTB			0x14
#define OST_REG_OST1CNT			0x18
#define OST_REG_OST2CNTL		0x20
#define OST_REG_OSTCNT2HBUF		0x24
#define OST_REG_OSTESR			0x34
#define OST_REG_OSTECR			0x38

/* bits within the OSTCCR register */
#define OSTCCR_PRESCALE1_MASK	0x3
#define OSTCCR_PRESCALE2_MASK	0xc
#define OSTCCR_PRESCALE1_LSB	0
#define OSTCCR_PRESCALE2_LSB	2

/* bits within the OSTCR register */
#define OSTCR_OST1CLR			BIT(0)
#define OSTCR_OST2CLR			BIT(1)

/* bits within the OSTFR register */
#define OSTFR_FFLAG				BIT(0)

/* bits within the OSTMR register */
#define OSTMR_FMASK				BIT(0)

/* bits within the OSTESR register */
#define OSTESR_OST1ENS			BIT(0)
#define OSTESR_OST2ENS			BIT(1)

/* bits within the OSTECR register */
#define OSTECR_OST1ENC			BIT(0)
#define OSTECR_OST2ENC			BIT(1)

static DEFINE_PER_CPU(call_single_data_t, ingenic_cevt_csd);

enum ingenic_ost_version {
	ID_X1000,
	ID_X2000,
};

struct ingenic_soc_info {
	enum ingenic_ost_version version;
	const struct ingenic_ost_clk_info *clk_info;

	unsigned int num_channels;
	unsigned int base_offset;
};

struct ingenic_ost_clk_info {
	struct clk_init_data init_data;
	unsigned int idx;
	u32 ostccr_reg;
	u32 ostcntl_reg;
};

struct ingenic_ost_clk {
	struct clk_hw hw;
	unsigned int idx;
	struct ingenic_ost *ost;
	const struct ingenic_ost_clk_info *info;
};

struct ingenic_ost_timer {
	void __iomem *base;
	unsigned int cpu;
	unsigned int channel;
	struct clock_event_device cevt;
	struct clk *clk;
	char name[20];
};

struct ingenic_ost {
	void __iomem *base;
	const struct ingenic_soc_info *soc_info;
	struct clk *clk, *global_timer_clk;
	struct device_node *np;
	struct clocksource cs;

	struct clk_hw_onecell_data *clocks;
	struct ingenic_ost_timer timers[];
};

static struct ingenic_ost *ingenic_ost;

static inline struct ingenic_ost_clk *to_ost_clk(struct clk_hw *hw)
{
	return container_of(hw, struct ingenic_ost_clk, hw);
}

static unsigned long ingenic_ost_percpu_timer_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct ingenic_ost_clk *ost_clk = to_ost_clk(hw);
	const struct ingenic_ost_clk_info *info = ost_clk->info;
	unsigned int prescale;

	prescale = readl(ost_clk->ost->timers[info->idx].base + info->ostccr_reg);

	prescale = (prescale & OSTCCR_PRESCALE1_MASK) >> OSTCCR_PRESCALE1_LSB;

	return parent_rate >> (prescale * 2);
}

static unsigned long ingenic_ost_global_timer_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct ingenic_ost_clk *ost_clk = to_ost_clk(hw);
	const struct ingenic_ost_clk_info *info = ost_clk->info;
	unsigned int prescale;

	prescale = readl(ost_clk->ost->timers[info->idx].base + info->ostccr_reg);

	prescale = (prescale & OSTCCR_PRESCALE1_MASK) >> OSTCCR_PRESCALE1_LSB;

	return parent_rate >> (prescale * 2);
}

static u8 ingenic_ost_get_prescale(unsigned long rate, unsigned long req_rate)
{
	u8 prescale;

	for (prescale = 0; prescale < 2; prescale++)
		if ((rate >> (prescale * 2)) <= req_rate)
			return prescale;

	return 2; /* /16 divider */
}

static long ingenic_ost_round_rate(struct clk_hw *hw, unsigned long req_rate,
		unsigned long *parent_rate)
{
	unsigned long rate = *parent_rate;
	u8 prescale;

	if (req_rate > rate)
		return rate;

	prescale = ingenic_ost_get_prescale(rate, req_rate);

	return rate >> (prescale * 2);
}

static int ingenic_ost_percpu_timer_set_rate(struct clk_hw *hw, unsigned long req_rate,
		unsigned long parent_rate)
{
	struct ingenic_ost_clk *ost_clk = to_ost_clk(hw);
	const struct ingenic_ost_clk_info *info = ost_clk->info;
	u8 prescale = ingenic_ost_get_prescale(parent_rate, req_rate);
	int val;

	val = readl(ost_clk->ost->timers[info->idx].base + info->ostccr_reg);
	val = (val & ~OSTCCR_PRESCALE1_MASK) | (prescale << OSTCCR_PRESCALE1_LSB);
	writel(val, ost_clk->ost->timers[info->idx].base + info->ostccr_reg);

	return 0;
}

static int ingenic_ost_global_timer_set_rate(struct clk_hw *hw, unsigned long req_rate,
		unsigned long parent_rate)
{
	struct ingenic_ost_clk *ost_clk = to_ost_clk(hw);
	const struct ingenic_ost_clk_info *info = ost_clk->info;
	u8 prescale = ingenic_ost_get_prescale(parent_rate, req_rate);
	int val;

	val = readl(ost_clk->ost->timers[info->idx].base + info->ostccr_reg);
	if (ost_clk->ost->soc_info->version >= ID_X2000)
		val = (val & ~OSTCCR_PRESCALE1_MASK) | (prescale << OSTCCR_PRESCALE1_LSB);
	else
		val = (val & ~OSTCCR_PRESCALE2_MASK) | (prescale << OSTCCR_PRESCALE2_LSB);
	writel(val, ost_clk->ost->timers[info->idx].base + info->ostccr_reg);

	return 0;
}

static const struct clk_ops ingenic_ost_percpu_timer_ops = {
	.recalc_rate	= ingenic_ost_percpu_timer_recalc_rate,
	.round_rate		= ingenic_ost_round_rate,
	.set_rate		= ingenic_ost_percpu_timer_set_rate,
};

static const struct clk_ops ingenic_ost_global_timer_ops = {
	.recalc_rate	= ingenic_ost_global_timer_recalc_rate,
	.round_rate		= ingenic_ost_round_rate,
	.set_rate		= ingenic_ost_global_timer_set_rate,
};

static const char * const ingenic_ost_clk_parents[] = { "ext" };

static const struct ingenic_ost_clk_info x1000_ost_clk_info[] = {
	[OST_CLK_GLOBAL_TIMER] = {
		.init_data = {
			.name = "global timer",
			.parent_names = ingenic_ost_clk_parents,
			.num_parents = ARRAY_SIZE(ingenic_ost_clk_parents),
			.ops = &ingenic_ost_global_timer_ops,
			.flags = CLK_SET_RATE_UNGATE,
		},
		.ostccr_reg = OST_REG_OSTCCR,
		.ostcntl_reg = OST_REG_OST2CNTL,
	},

	[OST_CLK_PERCPU_TIMER] = {
		.init_data = {
			.name = "percpu timer",
			.parent_names = ingenic_ost_clk_parents,
			.num_parents = ARRAY_SIZE(ingenic_ost_clk_parents),
			.ops = &ingenic_ost_percpu_timer_ops,
			.flags = CLK_SET_RATE_UNGATE,
		},
		.idx = 0,
		.ostccr_reg = OST_REG_OSTCCR,
	},
};

static const struct ingenic_ost_clk_info x2000_ost_clk_info[] = {
	[OST_CLK_GLOBAL_TIMER] = {
		.init_data = {
			.name = "global timer",
			.parent_names = ingenic_ost_clk_parents,
			.num_parents = ARRAY_SIZE(ingenic_ost_clk_parents),
			.ops = &ingenic_ost_global_timer_ops,
			.flags = CLK_SET_RATE_UNGATE,
		},
		.ostccr_reg = OST_REG_OSTCCR,
		.ostcntl_reg = OST_REG_OSTCNTL,
	},

	[OST_CLK_PERCPU_TIMER0] = {
		.init_data = {
			.name = "percpu timer0",
			.parent_names = ingenic_ost_clk_parents,
			.num_parents = ARRAY_SIZE(ingenic_ost_clk_parents),
			.ops = &ingenic_ost_percpu_timer_ops,
			.flags = CLK_SET_RATE_UNGATE,
		},
		.idx = 0,
		.ostccr_reg = OST_REG_OSTCCR,
	},

	[OST_CLK_PERCPU_TIMER1] = {
		.init_data = {
			.name = "percpu timer1",
			.parent_names = ingenic_ost_clk_parents,
			.num_parents = ARRAY_SIZE(ingenic_ost_clk_parents),
			.ops = &ingenic_ost_percpu_timer_ops,
			.flags = CLK_SET_RATE_UNGATE,
		},
		.idx = 1,
		.ostccr_reg = OST_REG_OSTCCR,
	},
};

static u64 notrace ingenic_ost_global_timer_read_cntl(void)
{
	struct ingenic_ost *ost = ingenic_ost;
	unsigned int count;

	count = readl(ost->base + ost->soc_info->clk_info->ostcntl_reg);

	return count;
}

static u64 notrace ingenic_ost_clocksource_read(struct clocksource *cs)
{
	return ingenic_ost_global_timer_read_cntl();
}

static inline struct ingenic_ost *to_ingenic_ost(struct ingenic_ost_timer *timer)
{
	return container_of(timer, struct ingenic_ost, timers[timer->cpu]);
}

static inline struct ingenic_ost_timer *
to_ingenic_ost_timer(struct clock_event_device *evt)
{
	return container_of(evt, struct ingenic_ost_timer, cevt);
}

static int ingenic_ost_cevt_set_state_shutdown(struct clock_event_device *evt)
{
	struct ingenic_ost_timer *timer = to_ingenic_ost_timer(evt);
	struct ingenic_ost *ost = ingenic_ost;

	if (ost->soc_info->version >= ID_X2000)
		writel(0, timer->base + OST_REG_OSTER);
	else
		writel(OSTECR_OST1ENC, timer->base + OST_REG_OSTECR);

	return 0;
}

static int ingenic_ost_cevt_set_next(unsigned long next,
				     struct clock_event_device *evt)
{
	struct ingenic_ost_timer *timer = to_ingenic_ost_timer(evt);
	struct ingenic_ost *ost = ingenic_ost;

	writel((u32)~OSTFR_FFLAG, timer->base + OST_REG_OSTFR);
	writel(next, timer->base + OST_REG_OST1DFR);
	writel(OSTCR_OST1CLR, timer->base + OST_REG_OSTCR);
	if (ost->soc_info->version >= ID_X2000) {
		writel(OSTESR_OST1ENS, timer->base + OST_REG_OSTER);
	} else {
		writel(OSTESR_OST1ENS, timer->base + OST_REG_OSTESR);
		writel((u32)~OSTMR_FMASK, timer->base + OST_REG_OSTMR);
	}

	return 0;
}

static void ingenic_per_cpu_event_handler(void *info)
{
	struct clock_event_device *cevt = (struct clock_event_device *) info;

	cevt->event_handler(cevt);
}

static irqreturn_t ingenic_ost_cevt_cb(int irq, void *dev_id)
{
	struct ingenic_ost_timer *timer = dev_id;
	struct ingenic_ost *ost = ingenic_ost;
	call_single_data_t *csd;

	if (ost->soc_info->version >= ID_X2000)
		writel(0, timer->base + OST_REG_OSTER);
	else
		writel(OSTECR_OST1ENC, timer->base + OST_REG_OSTECR);

	if (timer->cevt.event_handler) {
		csd = &per_cpu(ingenic_cevt_csd, timer->cpu);
		csd->info = (void *) &timer->cevt;
		csd->func = ingenic_per_cpu_event_handler;
		smp_call_function_single_async(timer->cpu, csd);
	}

	return IRQ_HANDLED;
}

static int __init ingenic_ost_register_clock(struct ingenic_ost *ost,
			unsigned int idx, const struct ingenic_ost_clk_info *info,
			struct clk_hw_onecell_data *clocks)
{
	struct ingenic_ost_clk *ost_clk;
	int val, err;

	ost_clk = kzalloc(sizeof(*ost_clk), GFP_KERNEL);
	if (!ost_clk)
		return -ENOMEM;

	ost_clk->hw.init = &info->init_data;
	ost_clk->idx = idx;
	ost_clk->info = info;
	ost_clk->ost = ost;

	/* Reset clock divider */
	val = readl(ost->timers[info->idx].base + info->ostccr_reg);
	val &= ~(OSTCCR_PRESCALE1_MASK);
	writel(val, ost->timers[info->idx].base + info->ostccr_reg);

	err = clk_hw_register(NULL, &ost_clk->hw);
	if (err) {
		kfree(ost_clk);
		return err;
	}

	clocks->hws[idx] = &ost_clk->hw;

	return 0;
}

static struct clk * __init ingenic_ost_get_clock(struct device_node *np, int id)
{
	struct of_phandle_args args;

	args.np = np;
	args.args_count = 1;
	args.args[0] = id;

	return of_clk_get_from_provider(&args);
}

static int __init ingenic_ost_setup_cevt(unsigned int cpu)
{
	struct ingenic_ost *ost = ingenic_ost;
	struct ingenic_ost_timer *timer = &ost->timers[cpu];
	unsigned int timer_virq;
	unsigned long rate;
	int err;

	timer->clk = ingenic_ost_get_clock(ost->np, timer->channel);
	if (IS_ERR(timer->clk))
		return PTR_ERR(timer->clk);

	err = clk_prepare_enable(timer->clk);
	if (err)
		goto err_clk_put;

	rate = clk_get_rate(timer->clk);
	if (!rate) {
		err = -EINVAL;
		goto err_clk_disable;
	}

	timer_virq = of_irq_get(ost->np, 0);
	if (!timer_virq) {
		err = -EINVAL;
		goto err_clk_disable;
	}

	snprintf(timer->name, sizeof(timer->name), "OST percpu timer%u", cpu);

	err = request_irq(timer_virq, ingenic_ost_cevt_cb, IRQF_TIMER,
			  timer->name, timer);
	if (err)
		goto err_irq_dispose_mapping;

	/* Unmask full comparison match interrupt */
	writel((u32)~OSTMR_FMASK, timer->base + OST_REG_OSTMR);

	timer->cpu = smp_processor_id();
	timer->cevt.cpumask = cpumask_of(smp_processor_id());
	timer->cevt.features = CLOCK_EVT_FEAT_ONESHOT;
	timer->cevt.name = timer->name;
	timer->cevt.rating = 400;
	timer->cevt.set_state_shutdown = ingenic_ost_cevt_set_state_shutdown;
	timer->cevt.set_next_event = ingenic_ost_cevt_set_next;

	clockevents_config_and_register(&timer->cevt, rate, 4, 0xffffffff);

	return 0;

err_irq_dispose_mapping:
	irq_dispose_mapping(timer_virq);
err_clk_disable:
	clk_disable_unprepare(timer->clk);
err_clk_put:
	clk_put(timer->clk);
	return err;
}

static int __init ingenic_ost_global_timer_init(struct device_node *np,
					       struct ingenic_ost *ost)
{
	unsigned int channel = OST_CLK_GLOBAL_TIMER;
	struct clocksource *cs = &ost->cs;
	unsigned long rate;
	int err;

	ost->global_timer_clk = ingenic_ost_get_clock(np, channel);
	if (IS_ERR(ost->global_timer_clk))
		return PTR_ERR(ost->global_timer_clk);

	err = clk_prepare_enable(ost->global_timer_clk);
	if (err)
		goto err_clk_put;

	rate = clk_get_rate(ost->global_timer_clk);
	if (!rate) {
		err = -EINVAL;
		goto err_clk_disable;
	}

	/* Clear counter CNT registers and enable OST channel */
	if (ost->soc_info->version >= ID_X2000) {
		writel(OSTCR_OST1CLR, ost->base + OST_REG_OSTCR);
		writel(OSTESR_OST1ENS, ost->base + OST_REG_OSTER);
	} else {
		writel(OSTCR_OST2CLR, ost->base + OST_REG_OSTCR);
		writel(OSTESR_OST2ENS, ost->base + OST_REG_OSTESR);
	}

	cs->name = "ingenic-ost";
	cs->rating = 400;
	cs->flags = CLOCK_SOURCE_IS_CONTINUOUS;
	cs->mask = CLOCKSOURCE_MASK(32);
	cs->read = ingenic_ost_clocksource_read;

	err = clocksource_register_hz(cs, rate);
	if (err)
		goto err_clk_disable;

	return 0;

err_clk_disable:
	clk_disable_unprepare(ost->global_timer_clk);
err_clk_put:
	clk_put(ost->global_timer_clk);
	return err;
}

static const struct ingenic_soc_info x1000_soc_info = {
	.version = ID_X1000,
	.clk_info = x1000_ost_clk_info,

	.num_channels = 2,
};

static const struct ingenic_soc_info x2000_soc_info = {
	.version = ID_X2000,
	.clk_info = x2000_ost_clk_info,

	.num_channels = 3,
	.base_offset = 0x100,
};

static const struct of_device_id __maybe_unused ingenic_ost_of_matches[] __initconst = {
	{ .compatible = "ingenic,x1000-ost", .data = &x1000_soc_info },
	{ .compatible = "ingenic,x2000-ost", .data = &x2000_soc_info },
	{ /* sentinel */ }
};

static int __init ingenic_ost_probe(struct device_node *np)
{
	const struct of_device_id *id = of_match_node(ingenic_ost_of_matches, np);
	struct ingenic_ost_timer *timer;
	struct ingenic_ost *ost;
	void __iomem *base;
	unsigned int cpu;
	unsigned int i;
	int ret;

	ost = kzalloc(struct_size(ost, timers, num_possible_cpus()), GFP_KERNEL);
	if (!ost)
		return -ENOMEM;

	ost->np = np;
	ost->soc_info = id->data;

	ost->base = of_io_request_and_map(np, 0, of_node_full_name(np));
	if (IS_ERR(ost->base)) {
		pr_err("%s: Failed to map OST registers\n", __func__);
		ret = PTR_ERR(ost->base);
		goto err_free_ost;
	}

	if (ost->soc_info->version >= ID_X2000) {
		base = of_io_request_and_map(np, 1, of_node_full_name(np));
		if (IS_ERR(base)) {
			pr_err("%s: Failed to map OST registers\n", __func__);
			ret = PTR_ERR(ost->base);
			goto err_free_ost;
		}
	}

	ost->clk = of_clk_get_by_name(np, "ost");
	if (IS_ERR(ost->clk)) {
		ret = PTR_ERR(ost->clk);
		pr_crit("%s: Cannot get OST clock\n", __func__);
		goto err_free_ost;
	}

	ret = clk_prepare_enable(ost->clk);
	if (ret) {
		pr_crit("%s: Unable to enable OST clock\n", __func__);
		goto err_put_clk;
	}

	ost->clocks = kzalloc(struct_size(ost->clocks, hws, ost->soc_info->num_channels),
			      GFP_KERNEL);
	if (!ost->clocks) {
		ret = -ENOMEM;
		goto err_clk_disable;
	}

	ost->clocks->num = ost->soc_info->num_channels;

	for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
		timer = &ost->timers[cpu];

		if (ost->soc_info->version >= ID_X2000)
			timer->base = base + ost->soc_info->base_offset * cpu;
		else
			timer->base = ost->base;

		timer->cpu = cpu;
		timer->channel = OST_CLK_PERCPU_TIMER + cpu;
	}

	for (i = 0; i < num_possible_cpus() + 1; i++) {
		ret = ingenic_ost_register_clock(ost, i, &ost->soc_info->clk_info[i], ost->clocks);
		if (ret) {
			pr_crit("%s: Cannot register clock %d\n", __func__, i);
			goto err_unregister_ost_clocks;
		}
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, ost->clocks);
	if (ret) {
		pr_crit("%s: Cannot add OF clock provider\n", __func__);
		goto err_unregister_ost_clocks;
	}

	ingenic_ost = ost;

	return 0;

err_unregister_ost_clocks:
	for (i = 0; i < ost->clocks->num; i++)
		if (ost->clocks->hws[i])
			clk_hw_unregister(ost->clocks->hws[i]);
	kfree(ost->clocks);
err_clk_disable:
	clk_disable_unprepare(ost->clk);
err_put_clk:
	clk_put(ost->clk);
err_free_ost:
	kfree(ost);
	return ret;
}

static int __init ingenic_ost_init(struct device_node *np)
{
	struct ingenic_ost *ost;
	unsigned long rate;
	int ret;

	ret = ingenic_ost_probe(np);
	if (ret) {
		pr_crit("%s: Failed to initialize OST clocks: %d\n", __func__, ret);
		return ret;
	}

	of_node_clear_flag(np, OF_POPULATED);

	ost = ingenic_ost;
	if (IS_ERR(ost))
		return PTR_ERR(ost);

	ret = ingenic_ost_global_timer_init(np, ost);
	if (ret) {
		pr_crit("%s: Unable to init global timer: %x\n", __func__, ret);
		goto err_free_ingenic_ost;
	}

	/* Setup clock events on each CPU core */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "Ingenic XBurst: online",
				ingenic_ost_setup_cevt, NULL);
	if (ret < 0) {
		pr_crit("%s: Unable to init percpu timers: %x\n", __func__, ret);
		goto err_ost_global_timer_cleanup;
	}

	/* Register the sched_clock at the end as there's no way to undo it */
	rate = clk_get_rate(ost->global_timer_clk);
	sched_clock_register(ingenic_ost_global_timer_read_cntl, 32, rate);

	return 0;

err_ost_global_timer_cleanup:
	clocksource_unregister(&ost->cs);
	clk_disable_unprepare(ost->global_timer_clk);
	clk_put(ost->global_timer_clk);
err_free_ingenic_ost:
	kfree(ost);
	return ret;
}

TIMER_OF_DECLARE(x1000_ost,  "ingenic,x1000-ost",  ingenic_ost_init);
TIMER_OF_DECLARE(x2000_ost,  "ingenic,x2000-ost",  ingenic_ost_init);
