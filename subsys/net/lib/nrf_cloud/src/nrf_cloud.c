/*
 * Copyright (c) 2017 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#if defined(CONFIG_POSIX_API)
#include <zephyr/posix/poll.h>
#else
#include <zephyr/net/socket.h>
#endif
#include <net/nrf_cloud.h>
#include <zephyr/net/mqtt.h>
#include "nrf_cloud_codec.h"
#include "nrf_cloud_fsm.h"
#include "nrf_cloud_transport.h"
#include "nrf_cloud_fota.h"
#include "nrf_cloud_mem.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf_cloud, CONFIG_NRF_CLOUD_LOG_LEVEL);

/* Flag to indicate if a disconnect has been requested. */
static atomic_t disconnect_requested;

/* Flag to indicate if a transport disconnect event has been received. */
static atomic_t transport_disconnected;

/* Flag to indicate if uninit/cleanup is in progress. */
static atomic_t uninit_in_progress;
static K_SEM_DEFINE(uninit_disconnect, 0, 1);

/* Handler registered by the application with the module to receive
 * asynchronous events.
 */
static nrf_cloud_event_handler_t app_event_handler;

/* Maintains the state with respect to the cloud. */
static volatile enum nfsm_state current_state = STATE_IDLE;
static K_MUTEX_DEFINE(state_mutex);

#if IS_ENABLED(CONFIG_NRF_CLOUD_CONNECTION_POLL_THREAD)
static K_SEM_DEFINE(connection_poll_sem, 0, 1);
static atomic_t connection_poll_active;
static int start_connection_poll();
#endif

enum nfsm_state nfsm_get_current_state(void)
{
	return current_state;
}

void nfsm_set_current_state_and_notify(enum nfsm_state state,
				       const struct nrf_cloud_evt *evt)
{
	bool discon_evt = (evt != NULL) &&
			  (evt->type == NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED);

	k_mutex_lock(&state_mutex, K_FOREVER);

	if (!atomic_get(&uninit_in_progress)) {
		LOG_DBG("state: %d", state);
		current_state = state;
	}

	k_mutex_unlock(&state_mutex);

	if (discon_evt) {
		atomic_set(&transport_disconnected, 1);
	}

	if ((app_event_handler != NULL) && (evt != NULL)) {
		app_event_handler(evt);
	}

	if (discon_evt && atomic_get(&uninit_in_progress)) {
		/* User has been notified of disconnect, continue with un-init */
		k_sem_give(&uninit_disconnect);
	}
}

bool nfsm_get_disconnect_requested(void)
{
	return (bool)atomic_get(&disconnect_requested);
}

int nrf_cloud_init(const struct nrf_cloud_init_param *param)
{
	int err;

	if (current_state != STATE_IDLE ||
	    atomic_get(&uninit_in_progress)) {
		return -EACCES;
	}

	if (param->event_handler == NULL) {
		return -EINVAL;
	}

	/* Initialize the state machine. */
	err = nfsm_init();
	if (err) {
		return err;
	}
	/* Initialize the encoder, decoder unit. */
	err = nrf_cloud_codec_init();
	if (err) {
		return err;
	}

	/* Set the flash device before initializing the transport/FOTA. */
#if defined(CONFIG_NRF_CLOUD_FOTA_FULL_MODEM_UPDATE)
	if (param->fmfu_dev_inf) {
		err = nrf_cloud_fota_fmfu_dev_set(param->fmfu_dev_inf);
		if (err < 0) {
			return err;
		}
	} else {
		LOG_WRN("Full modem FOTA not initialized; flash device info not provided");
	}
#endif

	/* Initialize the transport. */
	err = nct_init(param->client_id);
	if (err) {
		return err;
	}

	app_event_handler = param->event_handler;

	nfsm_set_current_state_and_notify(STATE_INITIALIZED, NULL);

	return 0;
}

int nrf_cloud_uninit(void)
{
	int err = 0;
	enum nfsm_state prev_state;

#if defined(CONFIG_NRF_CLOUD_FOTA)
	err = nrf_cloud_fota_uninit();
	if (err == -EBUSY) {
		LOG_WRN("Cannot uninitialize while a FOTA job is active");
		return -EBUSY;
	}
#endif

	atomic_set(&uninit_in_progress, 1);

	k_mutex_lock(&state_mutex, K_FOREVER);
	/* Save state and set to idle to prevent other API calls */
	prev_state = current_state;
	current_state = STATE_IDLE;
	k_mutex_unlock(&state_mutex);

	if (prev_state >= STATE_CONNECTED) {
		LOG_DBG("Disconnecting from nRF Cloud");

		atomic_set(&disconnect_requested, 1);
		k_sem_reset(&uninit_disconnect);
		(void)nct_disconnect();

		err = k_sem_take(&uninit_disconnect, K_SECONDS(30));
		if (err == -EAGAIN) {
			LOG_WRN("Did not receive expected disconnect event during cloud unint");
			err = -EISCONN;
		}
	}

	LOG_DBG("Cleaning up nRF Cloud resources");
	app_event_handler = NULL;
	nct_uninit();

	atomic_set(&uninit_in_progress, 0);
	return err;
}

static int connect_error_translate(const int err)
{
	switch (err) {
	case 0:
		return NRF_CLOUD_CONNECT_RES_SUCCESS;
	case -ECHILD:
		return NRF_CLOUD_CONNECT_RES_ERR_NETWORK;
	case -EACCES:
		return NRF_CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case -ENOEXEC:
		return NRF_CLOUD_CONNECT_RES_ERR_BACKEND;
	case -EINVAL:
		return NRF_CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case -EOPNOTSUPP:
		return NRF_CLOUD_CONNECT_RES_ERR_CERT;
	case -ECONNREFUSED:
		return NRF_CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case -ETIMEDOUT:
		return NRF_CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case -ENOMEM:
		return NRF_CLOUD_CONNECT_RES_ERR_NO_MEM;
	case -EINPROGRESS:
		return NRF_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("nRF cloud connect failed %d", err);
		return NRF_CLOUD_CONNECT_RES_ERR_MISC;
	}
}

static int connect_to_cloud(void)
{
	atomic_set(&disconnect_requested, 0);
	return nct_connect();
}

int nrf_cloud_connect(const struct nrf_cloud_connect_param *param)
{
	int err;

	if (current_state == STATE_IDLE) {
		return NRF_CLOUD_CONNECT_RES_ERR_NOT_INITD;
	} else if (current_state != STATE_INITIALIZED) {
		return NRF_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	}

#if IS_ENABLED(CONFIG_NRF_CLOUD_CONNECTION_POLL_THREAD)
	err = start_connection_poll();
#else
	err = connect_to_cloud();
	if (!err) {
		atomic_set(&transport_disconnected,0);
	}
#endif
	return connect_error_translate(err);
}

int nrf_cloud_disconnect(void)
{
	if (current_state < STATE_CONNECTED) {
		return -EACCES;
	}

	atomic_set(&disconnect_requested, 1);
	return nct_disconnect();
}

int nrf_cloud_shadow_update(const struct nrf_cloud_sensor_data *param)
{
	int err;
	struct nct_cc_data sensor_data = {
		.opcode = NCT_CC_OPCODE_UPDATE_REQ,
	};

	if (current_state != STATE_DC_CONNECTED) {
		return -EACCES;
	}

	if (param == NULL) {
		return -EINVAL;
	}

	if (IS_VALID_USER_TAG(param->tag)) {
		sensor_data.message_id = param->tag;
	} else {
		sensor_data.message_id = NCT_MSG_ID_USE_NEXT_INCREMENT;
	}

	err = nrf_cloud_encode_shadow_data(param, &sensor_data.data);
	if (err) {
		return err;
	}

	err = nct_cc_send(&sensor_data);
	nrf_cloud_free((void *)sensor_data.data.ptr);

	return err;
}

int nrf_cloud_shadow_device_status_update(const struct nrf_cloud_device_status *const dev_status)
{
	int err = 0;
	struct nrf_cloud_tx_data tx_data = {
		.topic_type = NRF_CLOUD_TOPIC_STATE,
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	};

	if (current_state != STATE_DC_CONNECTED) {
		return -EACCES;
	}

	err = nrf_cloud_device_status_encode(dev_status, &tx_data.data, true);
	if (err) {
		return err;
	}

	err = nrf_cloud_send(&tx_data);

	nrf_cloud_device_status_free(&tx_data.data);

	return err;
}

int nrf_cloud_sensor_data_send(const struct nrf_cloud_sensor_data *param)
{
	int err;
	struct nct_dc_data sensor_data;

	if (current_state != STATE_DC_CONNECTED) {
		return -EACCES;
	}

	if (param == NULL) {
		return -EINVAL;
	}

	err = nrf_cloud_encode_sensor_data(param, &sensor_data.data);
	if (err) {
		return err;
	}

	if (IS_VALID_USER_TAG(param->tag)) {
		sensor_data.message_id = param->tag;
	} else {
		sensor_data.message_id = NCT_MSG_ID_USE_NEXT_INCREMENT;
	}

	err = nct_dc_send(&sensor_data);
	nrf_cloud_free((void *)sensor_data.data.ptr);

	return err;
}

int nrf_cloud_sensor_data_stream(const struct nrf_cloud_sensor_data *param)
{
	int err;
	struct nct_dc_data sensor_data;

	if (current_state != STATE_DC_CONNECTED) {
		return -EACCES;
	}

	if (param == NULL) {
		return -EINVAL;
	}

	err = nrf_cloud_encode_sensor_data(param, &sensor_data.data);
	if (err) {
		return err;
	}

	err = nct_dc_stream(&sensor_data);
	nrf_cloud_free((void *)sensor_data.data.ptr);

	return err;
}

int nrf_cloud_send(const struct nrf_cloud_tx_data *msg)
{
	int err;

	if (!msg) {
		return -EINVAL;
	}

	switch (msg->topic_type) {
	case NRF_CLOUD_TOPIC_STATE: {
		if (current_state < STATE_CC_CONNECTED) {
			return -EACCES;
		}
		const struct nct_cc_data shadow_data = {
			.opcode = NCT_CC_OPCODE_UPDATE_REQ,
			.data.ptr = msg->data.ptr,
			.data.len = msg->data.len,
			.message_id = (msg->id > 0) ? msg->id : NCT_MSG_ID_USE_NEXT_INCREMENT
		};

		err = nct_cc_send(&shadow_data);
		if (err) {
			LOG_ERR("nct_cc_send failed, error: %d\n", err);
			return err;
		}

		break;
	}
	case NRF_CLOUD_TOPIC_MESSAGE: {
		if (current_state != STATE_DC_CONNECTED) {
			return -EACCES;
		}
		const struct nct_dc_data buf = {
			.data.ptr = msg->data.ptr,
			.data.len = msg->data.len,
			.message_id = (msg->id > 0) ? msg->id : NCT_MSG_ID_USE_NEXT_INCREMENT
		};

		if (msg->qos == MQTT_QOS_0_AT_MOST_ONCE) {
			err = nct_dc_stream(&buf);
		} else if (msg->qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			err = nct_dc_send(&buf);
		} else {
			err = -EINVAL;
			LOG_ERR("Unsupported QoS setting");
			return err;
		}

		break;
	}
	case NRF_CLOUD_TOPIC_BULK: {
		if (current_state != STATE_DC_CONNECTED) {
			return -EACCES;
		}
		const struct nct_dc_data buf = {
			.data.ptr = msg->data.ptr,
			.data.len = msg->data.len,
			.message_id = (msg->id > 0) ? msg->id : NCT_MSG_ID_USE_NEXT_INCREMENT
		};

		err = nct_dc_bulk_send(&buf, msg->qos);
		if (err) {
			LOG_ERR("nct_dc_bulk_send failed, error: %d", err);
			return err;
		}

		break;
	}
	default:
		LOG_ERR("Unknown topic type");
		return -ENODATA;
	}

	return 0;
}

int nrf_cloud_tenant_id_get(char *id_buf, size_t id_len)
{
	return nct_tenant_id_get(id_buf, id_len);
}

int nct_input(const struct nct_evt *evt)
{
	return nfsm_handle_incoming_event(evt, current_state);
}

void nct_send_event(const struct nrf_cloud_evt * const evt)
{
	__ASSERT_NO_MSG(evt != NULL);

	if (app_event_handler && evt) {
		app_event_handler(evt);
	}
}

int nrf_cloud_process(void)
{
	return nct_process();
}

#if IS_ENABLED(CONFIG_NRF_CLOUD_CONNECTION_POLL_THREAD)
static int start_connection_poll()
{
	if (current_state == STATE_IDLE) {
		return -EACCES;
	}

	if (atomic_get(&connection_poll_active)) {
		LOG_DBG("Connection poll in progress");
		return -EINPROGRESS;
	}

	atomic_set(&disconnect_requested, 0);
	k_sem_give(&connection_poll_sem);

	return 0;
}

void nrf_cloud_run(void)
{
	int ret;
	struct pollfd fds[1];
	struct nrf_cloud_evt evt;

start:
	k_sem_take(&connection_poll_sem, K_FOREVER);
	atomic_set(&connection_poll_active, 1);

	evt.type = NRF_CLOUD_EVT_TRANSPORT_CONNECTING;
	evt.status = NRF_CLOUD_CONNECT_RES_SUCCESS;
	nfsm_set_current_state_and_notify(nfsm_get_current_state(), &evt);

	ret = connect_to_cloud();
	ret = connect_error_translate(ret);

	if (ret != NRF_CLOUD_CONNECT_RES_SUCCESS) {
		evt.type = NRF_CLOUD_EVT_TRANSPORT_CONNECTING;
		evt.status = ret;
		nfsm_set_current_state_and_notify(nfsm_get_current_state(), &evt);
		goto reset;
	} else {
		LOG_DBG("Cloud connection request sent");
	}

	fds[0].fd = nct_socket_get();
	fds[0].events = POLLIN;

	/* Only disconnect events will occur below */
	evt.type = NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED;
	atomic_set(&transport_disconnected, 0);

	while (true) {
		ret = poll(fds, ARRAY_SIZE(fds), nct_keepalive_time_left());

		/* If poll returns 0 the timeout has expired. */
		if (ret == 0) {
			ret = nrf_cloud_process();
			if ((ret < 0) && (ret != -EAGAIN)) {
				LOG_DBG("Disconnecting; nrf_cloud_process returned an error: %d",
					ret);
				evt.status = NRF_CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
				break;
			}
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			ret = nrf_cloud_process();
			if ((ret < 0) && (ret != -EAGAIN)) {
				LOG_DBG("Disconnecting; nrf_cloud_process returned an error: %d",
					ret);
				evt.status = NRF_CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
				break;
			}

			if (atomic_get(&transport_disconnected) == 1) {
				LOG_DBG("The cloud socket is already closed");
				break;
			}

			continue;
		}

		if (ret < 0) {
			LOG_ERR("poll() returned an error: %d", ret);
			evt.status = NRF_CLOUD_DISCONNECT_MISC;
			break;
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			LOG_DBG("Socket error: POLLNVAL");
			if (nfsm_get_disconnect_requested()) {
				LOG_DBG("The cloud socket was disconnected by request");
				evt.status = NRF_CLOUD_DISCONNECT_USER_REQUEST;
			} else {
				LOG_DBG("The cloud socket was unexpectedly closed");
				evt.status = NRF_CLOUD_DISCONNECT_INVALID_REQUEST;
			}

			break;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_DBG("Socket error: POLLHUP");
			LOG_DBG("Connection was closed by the cloud");
			evt.status = NRF_CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
			break;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_DBG("Socket error: POLLERR");
			LOG_DBG("Cloud connection was unexpectedly closed");
			evt.status = NRF_CLOUD_DISCONNECT_MISC;
			break;
		}
	}

	/* Send the event if the transport has not already been disconnected */
	if (atomic_get(&transport_disconnected) == 0) {
		nfsm_set_current_state_and_notify(STATE_INITIALIZED, &evt);
		if (evt.status != NRF_CLOUD_DISCONNECT_USER_REQUEST) {
			/* Not requested, do proper disconnect */
			(void)nct_disconnect();
		}
	}

reset:
	atomic_set(&connection_poll_active, 0);
	k_sem_take(&connection_poll_sem, K_NO_WAIT);
	goto start;
}

#ifdef CONFIG_BOARD_QEMU_X86
#define POLL_THREAD_STACK_SIZE 4096
#else
#define POLL_THREAD_STACK_SIZE 3072
#endif
K_THREAD_DEFINE(nrfcloud_connection_poll_thread, POLL_THREAD_STACK_SIZE,
		nrf_cloud_run, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif
