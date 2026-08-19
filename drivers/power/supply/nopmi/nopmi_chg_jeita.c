// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "[nopmi_chg_jeita]: %s: " fmt, __func__

#include "nopmi_chg.h"

struct nopmi_chg_jeita_st *g_nopmi_chg_jeita;

bool g_ffc_disable = true;

static int nopmi_chg_jeita_get_batt_temperature(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->bms_psy) {
		pr_err("bms psy not available\n");
		return -EINVAL;
	}

	ret = power_supply_get_property(nopmi_chg_jeita->parent->bms_psy,
					POWER_SUPPLY_PROP_TEMP, &prop);
	if (ret < 0) {
		pr_err("couldn't read temperature property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("batt_temperature is %d\n", prop.intval / 10);
	return prop.intval / 10;
}

static int nopmi_chg_jeita_get_charger_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->bbc_psy) {
		pr_err("bbc psy not available\n");
		return -EINVAL;
	}

	ret = power_supply_get_property(nopmi_chg_jeita->parent->bbc_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't read voltage property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("voltage is %d\n", prop.intval);
	return prop.intval;
}

static int nopmi_chg_jeita_get_batt_id(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->bms_psy) {
		pr_err("bms psy not available\n");
		return -EINVAL;
	}

	ret = power_supply_get_property(nopmi_chg_jeita->parent->bms_psy,
					POWER_SUPPLY_PROP_RESISTANCE_ID, &prop);
	if (ret < 0) {
		pr_err("couldn't get batt_id property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("batt_id is %d\n", prop.intval);
	return prop.intval;
}

static int nopmi_chg_jeita_get_pd_active(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->usb_psy) {
		pr_err("usb psy not available\n");
		return -EINVAL;
	}

	ret = power_supply_get_property(nopmi_chg_jeita->parent->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE, &prop);
	if (ret < 0) {
		pr_err("couldn't get pd active property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("pd active is %d\n", prop.intval);
	return prop.intval;
}

static int nopmi_chg_jeita_get_charger_term_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->usb_psy) {
		pr_err("usb psy not available\n");
		return -EINVAL;
	}

	ret = power_supply_get_property(nopmi_chg_jeita->parent->usb_psy,
					POWER_SUPPLY_PROP_TERMINATION_CURRENT, &prop);
	if (ret < 0) {
		pr_err("couldn't get term_current property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("term_current is %d\n", prop.intval);
	return prop.intval;
}

static int nopmi_chg_jeita_set_charger_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita, int cc)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->bbc_psy) {
		pr_err("bbc psy not available\n");
		return -EINVAL;
	}

	prop.intval = cc;
	ret = power_supply_set_property(nopmi_chg_jeita->parent->bbc_psy,
					POWER_SUPPLY_PROP_CURRENT_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't set current property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("set current as %d\n", prop.intval);
	return 0;
}

static int nopmi_chg_jeita_set_charger_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita, int cv)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->bbc_psy) {
		pr_err("bbc psy not available\n");
		return -EINVAL;
	}

	prop.intval = cv;
	ret = power_supply_set_property(nopmi_chg_jeita->parent->bbc_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't set voltage property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("set voltage as %d\n", prop.intval);
	return 0;
}

static int nopmi_chg_jeita_set_charger_term_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita,
						    int term_curr)
{
	union power_supply_propval prop = {0, };
	int ret;

	if (!nopmi_chg_jeita->parent->usb_psy) {
		pr_err("usb psy not available\n");
		return -EINVAL;
	}

	prop.intval = term_curr;
	ret = power_supply_set_property(nopmi_chg_jeita->parent->usb_psy,
					POWER_SUPPLY_PROP_TERMINATION_CURRENT, &prop);
	if (ret < 0) {
		pr_err("couldn't set term_current property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("set term_current as %d\n", prop.intval);
	return 0;
}

static void nopmi_chg_handle_jeita_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int ret = 0;
	int chg1_cv = 0;
	int term_curr_pre = 0;
	int pd_active = 0;
	struct sw_jeita_data *sw_jeita = &nopmi_chg_jeita->sw_jeita;
	union power_supply_propval prop = {0, };
	bool cp_work_flag = false;

	sw_jeita->pre_sm = sw_jeita->sm;
	sw_jeita->charging = true;
	/* JEITA battery temp Standard */
	if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_thres) {
		pr_err("[SW_JEITA] Battery Over Temperature(%d)!!\n",
		       nopmi_chg_jeita->dt.temp_t4_thres);
		sw_jeita->sm = TEMP_ABOVE_T4;
		sw_jeita->charging = false;
	} else if (nopmi_chg_jeita->battery_temp > nopmi_chg_jeita->dt.temp_t3_thres) {
		/* control 45 degree to normal behavior */
		if (nopmi_chg_jeita->battery_temp >=
		    nopmi_chg_jeita->dt.temp_t4_thres_minus_x_degree) {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d, not allow charging yet!!\n",
			       nopmi_chg_jeita->dt.temp_t4_thres_minus_x_degree,
			       nopmi_chg_jeita->dt.temp_t4_thres);
			sw_jeita->charging = false;
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t3_thres,
			       nopmi_chg_jeita->dt.temp_t4_thres);
			sw_jeita->sm = TEMP_T3_TO_T4;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_t3_to_t4_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t2_thres) {
		if ((sw_jeita->sm == TEMP_T3_TO_T4 &&
		     nopmi_chg_jeita->battery_temp >=
		     nopmi_chg_jeita->dt.temp_t3_thres_minus_x_degree) ||
		    (sw_jeita->sm == TEMP_T1P5_TO_T2 &&
		     nopmi_chg_jeita->battery_temp <=
		     nopmi_chg_jeita->dt.temp_t2_thres_plus_x_degree)) {
			pr_err("[SW_JEITA] Battery Temperature not recovery to normal temperature charging mode yet!!\n");
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t2_thres,
			       nopmi_chg_jeita->dt.temp_t3_thres);
			sw_jeita->sm = TEMP_T2_TO_T3;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_t2_to_t3_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1p5_thres) {
		if ((sw_jeita->sm == TEMP_T1_TO_T1P5 || sw_jeita->sm == TEMP_T0_TO_T1) &&
		    nopmi_chg_jeita->battery_temp <=
		    nopmi_chg_jeita->dt.temp_t1p5_thres_plus_x_degree) {
			if (sw_jeita->sm == TEMP_T1_TO_T1P5) {
				pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
				       nopmi_chg_jeita->dt.temp_t1p5_thres_plus_x_degree,
				       nopmi_chg_jeita->dt.temp_t2_thres);
			}
			if (sw_jeita->sm == TEMP_T0_TO_T1) {
				pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
				       nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree,
				       nopmi_chg_jeita->dt.temp_t1p5_thres);
			}
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t1p5_thres,
			       nopmi_chg_jeita->dt.temp_t2_thres);
			sw_jeita->sm = TEMP_T1P5_TO_T2;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_t1p5_to_t2_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1_thres) {
		if ((sw_jeita->sm == TEMP_T0_TO_T1 ||
		     sw_jeita->sm == TEMP_BELOW_T0 ||
		     sw_jeita->sm == TEMP_TN1_TO_T0) &&
		    nopmi_chg_jeita->battery_temp <=
		    nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree) {
			if (sw_jeita->sm == TEMP_T0_TO_T1)
				pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
				       nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree,
				       nopmi_chg_jeita->dt.temp_t1p5_thres);

			if (sw_jeita->sm == TEMP_BELOW_T0) {
				pr_err("[SW_JEITA] Battery Temperature between %d and %d, not allow charging yet!!\n",
				       nopmi_chg_jeita->dt.temp_tn1_thres,
				       nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
				sw_jeita->charging = false;
			}
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t1_thres,
			       nopmi_chg_jeita->dt.temp_t1p5_thres);
			sw_jeita->sm = TEMP_T1_TO_T1P5;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_t1_to_t1p5_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t0_thres) {
		if ((sw_jeita->sm == TEMP_BELOW_T0 || sw_jeita->sm == TEMP_TN1_TO_T0) &&
		    nopmi_chg_jeita->battery_temp <=
		    nopmi_chg_jeita->dt.temp_t0_thres_plus_x_degree) {
			if (sw_jeita->sm == TEMP_BELOW_T0) {
				pr_err("[SW_JEITA] Battery Temperature between %d and %d, not allow charging yet!!\n",
				       nopmi_chg_jeita->dt.temp_tn1_thres,
				       nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
				sw_jeita->charging = false;
			} else if (sw_jeita->sm == TEMP_TN1_TO_T0) {
				pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
				       nopmi_chg_jeita->dt.temp_t0_thres_plus_x_degree,
				       nopmi_chg_jeita->dt.temp_tn1_thres);
			}
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t0_thres,
			       nopmi_chg_jeita->dt.temp_t1_thres);
			sw_jeita->sm = TEMP_T0_TO_T1;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_t0_to_t1_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_tn1_thres) {
		if (sw_jeita->sm == TEMP_BELOW_T0 &&
		    nopmi_chg_jeita->battery_temp <=
		    nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree) {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d, not allow charging yet!!\n",
			       nopmi_chg_jeita->dt.temp_tn1_thres,
			       nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
			sw_jeita->charging = false;
		} else {
			pr_err("[SW_JEITA] Battery Temperature between %d and %d\n",
			       nopmi_chg_jeita->dt.temp_t0_thres,
			       nopmi_chg_jeita->dt.temp_tn1_thres);
			sw_jeita->sm = TEMP_TN1_TO_T0;
			nopmi_chg_jeita->jeita_current_limit =
				nopmi_chg_jeita->dt.temp_tn1_to_t0_fcc;
		}
	} else {
		pr_err("[SW_JEITA] Battery Cold Temperature(%d)!!\n",
		       nopmi_chg_jeita->dt.temp_tn1_thres);
		sw_jeita->sm = TEMP_BELOW_T0;
		sw_jeita->charging = false;
	}

	if (nopmi_chg_jeita->parent->fcc_votable) {
		vote(nopmi_chg_jeita->parent->fcc_votable, JEITA_VOTER, true,
		     nopmi_chg_jeita->jeita_current_limit);
	} else {
		ret = nopmi_chg_jeita_set_charger_current(nopmi_chg_jeita,
							  nopmi_chg_jeita->jeita_current_limit);
	}

	/*add for update fastcharge mode, start*/
	if (nopmi_chg_jeita->parent->bms_psy) {
		ret = power_supply_get_property(nopmi_chg_jeita->parent->bms_psy,
						POWER_SUPPLY_PROP_FASTCHARGE_MODE, &prop);
		if (ret < 0)
			pr_err("get fastcharge mode fail\n");
		nopmi_chg_jeita->fast_charge_mode = prop.intval;
	}

	if (!g_ffc_disable && nopmi_chg_jeita->fast_charge_mode &&
	    sw_jeita->sm != TEMP_T2_TO_T3) {
		prop.intval = 0;
		nopmi_chg_jeita->fast_charge_mode = 0;
		power_supply_set_property(nopmi_chg_jeita->parent->bms_psy,
					  POWER_SUPPLY_PROP_FASTCHARGE_MODE, &prop);
	} else if (!g_ffc_disable && !nopmi_chg_jeita->fast_charge_mode &&
		   sw_jeita->sm == TEMP_T2_TO_T3) {
		prop.intval = 1;
		nopmi_chg_jeita->fast_charge_mode = 1;
		power_supply_set_property(nopmi_chg_jeita->parent->bms_psy,
					  POWER_SUPPLY_PROP_FASTCHARGE_MODE, &prop);
	}
	/* add for update fastcharge mode, end*/

	/* set CV after temperature changed */
	/* In normal range, we adjust CV dynamically */
	if (sw_jeita->sm != TEMP_T2_TO_T3) {
		if (sw_jeita->sm == TEMP_ABOVE_T4)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_above_t4_cv;
		else if (sw_jeita->sm == TEMP_T3_TO_T4)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv;
		else if (sw_jeita->sm == TEMP_T2_TO_T3)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_T1P5_TO_T2)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t1p5_to_t2_cv;
		else if (sw_jeita->sm == TEMP_T1_TO_T1P5)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t1_to_t1p5_cv;
		else if (sw_jeita->sm == TEMP_T0_TO_T1)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t0_to_t1_cv;
		else if (sw_jeita->sm == TEMP_TN1_TO_T0)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_tn1_to_t0_cv;
		else if (sw_jeita->sm == TEMP_BELOW_T0)
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_below_t0_cv;
		else
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
	} else {
		if (nopmi_chg_jeita->fast_charge_mode && !g_ffc_disable) {
			if (nopmi_get_charger_ic_type() == NOPMI_CHARGER_IC_MAXIM)
				sw_jeita->cv = 4470;
			else
				sw_jeita->cv = 4480;
		} else {
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		}
	}

	if (nopmi_chg_jeita->parent->cp_psy) {
		ret = power_supply_get_property(nopmi_chg_jeita->parent->cp_psy,
						POWER_SUPPLY_PROP_CHARGING_ENABLED, &prop);
		if (ret < 0) {
			pr_err("get cp_psy CHARGING_ENABLED prop fail (%d)\n", ret);
		} else {
			cp_work_flag = !!prop.intval;
			pr_info("cp_work_flag: %d\n", cp_work_flag);
		}
	} else {
		pr_err("cp_psy not available\n");
	}

	if (cp_work_flag) {
		if (nopmi_get_charger_ic_type() != NOPMI_CHARGER_IC_MAXIM) {
			sw_jeita->cv = 4608;
			pr_info("cp_work: set cv to: %d\n", sw_jeita->cv);
		}
	}

	if (nopmi_chg_jeita->parent->fv_votable)
		chg1_cv = get_effective_result(nopmi_chg_jeita->parent->fv_votable);
	else
		chg1_cv = nopmi_chg_jeita_get_charger_voltage(nopmi_chg_jeita);

	if (sw_jeita->cv != chg1_cv) {
		if (nopmi_chg_jeita->parent->fv_votable) {
			vote(nopmi_chg_jeita->parent->fv_votable, JEITA_VOTER, true, sw_jeita->cv);
		} else {
			ret = nopmi_chg_jeita_set_charger_voltage(nopmi_chg_jeita, sw_jeita->cv);
			if (ret < 0)
				pr_err("Couldn't set cv to %d, ret=%d\n", sw_jeita->cv, ret);
		}
	}

	if (nopmi_get_charger_ic_type() != NOPMI_CHARGER_IC_MAXIM) {
		pd_active = nopmi_chg_jeita_get_pd_active(nopmi_chg_jeita);
		if (sw_jeita->sm == TEMP_T2_TO_T3 && pd_active == 2) {
			if (nopmi_chg_jeita->battery_temp >= 35) {
				if (nopmi_chg_jeita->battery_id == 0 ||
				    nopmi_chg_jeita->battery_id == 1)
					sw_jeita->term_curr = 833;
				else
					sw_jeita->term_curr = 784;
			} else {
				sw_jeita->term_curr = 768;
			}
		} else {
			sw_jeita->term_curr = 256;
		}

		term_curr_pre = nopmi_chg_jeita_get_charger_term_current(nopmi_chg_jeita);
		if (sw_jeita->term_curr != term_curr_pre) {
			ret = nopmi_chg_jeita_set_charger_term_current(nopmi_chg_jeita,
								       sw_jeita->term_curr);
			if (ret < 0)
				pr_err("Couldn't set term_current to %d, ret=%d\n",
				       sw_jeita->term_curr, ret);
		}
	}
}

static void nopmi_chg_handle_jeita(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	struct sw_jeita_data *sw_jeita = &nopmi_chg_jeita->sw_jeita;

	nopmi_chg_jeita->battery_temp = nopmi_chg_jeita_get_batt_temperature(nopmi_chg_jeita);
	nopmi_chg_handle_jeita_current(nopmi_chg_jeita);
	if (!sw_jeita->charging) {
		sw_jeita->can_recharging = true;
		switch (nopmi_get_charger_ic_type()) {
		case NOPMI_CHARGER_IC_MAXIM:
			vote(nopmi_chg_jeita->parent->chgctrl_votable, JEITA_CHG_VOTER,
			     true, CHG_MODE_CHARGING_OFF);
			break;
		case NOPMI_CHARGER_IC_SYV:
			main_set_charge_enable(false);
			break;
		case NOPMI_CHARGER_IC_SC:
			break;
		default:
			break;
		}
	} else {
		if (sw_jeita->can_recharging) {
			switch (nopmi_get_charger_ic_type()) {
			case NOPMI_CHARGER_IC_MAXIM:
				vote(nopmi_chg_jeita->parent->chgctrl_votable, JEITA_CHG_VOTER,
				     false, CHG_MODE_CHARGING_OFF);
				break;
			case NOPMI_CHARGER_IC_SYV:
				main_set_charge_enable(true);
				break;
			case NOPMI_CHARGER_IC_SC:
				break;
			default:
				break;
			}
			sw_jeita->can_recharging = false;
		}
	}
}

static void nopmi_chg_jeita_workfunc(struct work_struct *work)
{
	struct nopmi_chg_jeita_st *chg_jeita =
		container_of(work, struct nopmi_chg_jeita_st, jeita_work.work);

	pr_info("star\n");
	chg_jeita->usb_present = nopmi_chg_is_usb_present(chg_jeita->parent->usb_psy);
	if (!chg_jeita->usb_present)
		return;

	/* skip elapsed_us debounce for handling battery temperature */
	if (chg_jeita->dt.enable_sw_jeita) {
		nopmi_chg_handle_jeita(chg_jeita);
		chg_jeita->sw_jeita_start = true;
	} else {
		chg_jeita->sw_jeita_start = false;
	}
}

void start_nopmi_chg_jeita_workfunc(struct nopmi_chg *chg)
{
	struct nopmi_chg_jeita_st *jeita = chg->jeita_st;

	if (!jeita)
		return;

	schedule_delayed_work(&jeita->jeita_work,
			      msecs_to_jiffies(JEITA_WORK_DELAY_MS));
}

void stop_nopmi_chg_jeita_workfunc(struct nopmi_chg *chg)
{
	struct nopmi_chg_jeita_st *jeita = chg->jeita_st;

	if (!jeita)
		return;

	cancel_delayed_work_sync(&jeita->jeita_work);

	if (chg->fcc_votable)
		vote(chg->fcc_votable, JEITA_VOTER, false, 0);
	if (chg->fv_votable)
		vote(chg->fv_votable, JEITA_VOTER, true, JEITA_TEMP_NORMAL_VOLTAGE);
}

static void nopmi_chg_jeita_state_init(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	struct sw_jeita_data *sw_jeita = &nopmi_chg_jeita->sw_jeita;

	if (nopmi_chg_jeita->dt.enable_sw_jeita) {
		nopmi_chg_jeita->battery_temp =
			nopmi_chg_jeita_get_batt_temperature(nopmi_chg_jeita);
		if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_thres)
			sw_jeita->sm = TEMP_ABOVE_T4;
		else if (nopmi_chg_jeita->battery_temp > nopmi_chg_jeita->dt.temp_t3_thres)
			sw_jeita->sm = TEMP_T3_TO_T4;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t2_thres)
			sw_jeita->sm = TEMP_T2_TO_T3;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1p5_thres)
			sw_jeita->sm = TEMP_T1P5_TO_T2;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1_thres)
			sw_jeita->sm = TEMP_T1_TO_T1P5;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t0_thres)
			sw_jeita->sm = TEMP_T0_TO_T1;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_tn1_thres)
			sw_jeita->sm = TEMP_TN1_TO_T0;
		else
			sw_jeita->sm = TEMP_BELOW_T0;

		pr_info("[SW_JEITA] temp: %d, sm: %d\n",
			nopmi_chg_jeita->battery_temp, sw_jeita->sm);
	}
}

int nopmi_chg_jeita_init(struct nopmi_chg *chg)
{
	struct nopmi_chg_jeita_st *jeita = chg->jeita_st;

	if (!jeita) {
		pr_err("jeita_st not allocated\n");
		return -EINVAL;
	}

	pr_info("entry\n");

	jeita->parent = chg;
	jeita->jeita_current_limit = TEMP_T2_TO_T3_FCC;
	jeita->fast_charge_mode = 0;

	INIT_DELAYED_WORK(&jeita->jeita_work, nopmi_chg_jeita_workfunc);
	nopmi_chg_jeita_state_init(jeita);
	if (nopmi_get_charger_ic_type() != NOPMI_CHARGER_IC_MAXIM)
		jeita->battery_id = nopmi_chg_jeita_get_batt_id(jeita);

	g_nopmi_chg_jeita = jeita;
	pr_info("done\n");
	return 0;
}

int nopmi_chg_jeita_deinit(struct nopmi_chg *chg)
{
	struct nopmi_chg_jeita_st *jeita = chg->jeita_st;

	if (!jeita)
		return 0;

	pr_info("entry\n");
	cancel_delayed_work_sync(&jeita->jeita_work);
	g_nopmi_chg_jeita = NULL;
	pr_info("done\n");

	return 0;
}
