/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <modem/lte_lc.h>
#include <net/socket.h>

#define TCP_IP_HEADER_SIZE 28

static int client_fd;
static struct sockaddr_storage host_addr;
static struct k_delayed_work server_transmission_work;

K_SEM_DEFINE(lte_connected, 0, 1);

static void server_transmission_work_fn(struct k_work *work)
{
	int err;
	char buffer[CONFIG_TCP_DATA_UPLOAD_SIZE_BYTES] = {"\0"};

	printk("Transmitting TCP/IP payload of %d bytes to the ",
	       CONFIG_TCP_DATA_UPLOAD_SIZE_BYTES + TCP_IP_HEADER_SIZE);
	printk("IP address %s, port number %d\n",
	       CONFIG_TCP_SERVER_ADDRESS_STATIC,
	       CONFIG_TCP_SERVER_PORT);

	err = send(client_fd, buffer, sizeof(buffer), 0);
	if (err < 0) {
		printk("Failed to transmit TCP packet, %d\n", errno);
		close(client_fd);
		return;
	}

	k_delayed_work_submit(
			&server_transmission_work,
			K_SECONDS(CONFIG_TCP_DATA_UPLOAD_FREQUENCY_SECONDS));
}

static void work_init(void)
{
	k_delayed_work_init(&server_transmission_work,
			    server_transmission_work_fn);
}

#if defined(CONFIG_LTE_LINK_CONTROL)
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		     (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		printk("Network registration status: %s\n",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming\n");
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		printk("PSM parameter update: TAU: %d, Active time: %d\n",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
			       "eDRX parameter update: eDRX: %f, PTW: %f\n",
			       evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if (len > 0) {
			printk("%s\n", log_buf);
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		printk("RRC mode: %s\n",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"Connected" : "Idle\n");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		printk("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
		       evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

static int configure_low_power(void)
{
	int err;

if (IS_ENABLED(CONFIG_TCP_PSM_ENABLE)) {
	/** Power Saving Mode */
	err = lte_lc_psm_req(true);
	if (err) {
		printk("lte_lc_psm_req, error: %d\n", err);
	}
} else {
	err = lte_lc_psm_req(false);
	if (err) {
		printk("lte_lc_psm_req, error: %d\n", err);
	}
}

if (IS_ENABLED(CONFIG_TCP_EDRX_ENABLE)) {
	/** enhanced Discontinuous Reception */
	err = lte_lc_edrx_req(true);
	if (err) {
		printk("lte_lc_edrx_req, error: %d\n", err);
	}
} else {
	err = lte_lc_edrx_req(false);
	if (err) {
		printk("lte_lc_edrx_req, error: %d\n", err);
	}
}

if (IS_ENABLED(CONFIG_TCP_RAI_ENABLE)) {
	/** Release Assistance Indication  */
	err = lte_lc_rai_req(true);
	if (err) {
		printk("lte_lc_rai_req, error: %d\n", err);
	}
}

	return err;
}

static void modem_configure(void)
{
	int err;

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already configured and LTE connected. */
	} else {
		err = lte_lc_init_and_connect_async(lte_handler);
		if (err) {
			printk("Modem configuration, error: %d\n", err);
			return;
		}
	}
}
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */

static void client_disconnect(void)
{
	(void)close(client_fd);
}

static int client_init(void)
{
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_TCP_SERVER_PORT);

	inet_pton(AF_INET, CONFIG_TCP_SERVER_ADDRESS_STATIC,
		  &server4->sin_addr);

	return 0;
}

static int client_connect(void)
{
	int err;

	client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client_fd < 0) {
		printk("Failed to create TCP socket: %d\n", errno);
		return 0;
	}

	err = connect(client_fd, (struct sockaddr *)&host_addr,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		printk("Connect failed : %d\n", errno);
		goto error;
	} else {
		printk("Successfully connected to TCP server: %s on port %d\n",
				CONFIG_TCP_SERVER_ADDRESS_STATIC, CONFIG_TCP_SERVER_PORT);
	}

	return 0;

error:
	client_disconnect();

	return err;
}

void main(void)
{
	int err;

	printk("TCP Client sample has started\n");

	work_init();

#if defined(CONFIG_LTE_LINK_CONTROL)
	err = configure_low_power();
	if (err) {
		printk("Unable to set low power configuration, error: %d\n",
		       err);
	}

	modem_configure();

	k_sem_take(&lte_connected, K_FOREVER);
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */

	err = client_init();
	if (err) {
		printk("Not able to initialize TCP server connection\n");
		return;
	}

	err = client_connect();
	if (err) {
		printk("Not able to connect to TCP server %s on port %d\n",
				CONFIG_TCP_SERVER_ADDRESS_STATIC, CONFIG_TCP_SERVER_PORT);
		return;
	}

	k_delayed_work_submit(&server_transmission_work, K_NO_WAIT);
}
