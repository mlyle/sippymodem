// SPDX-License-Identifier: BSD-3-Clause
#include <modem.h>
#include <modem_state.h>
#include <modem_frontend.h>
#include "test.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#define EXPECT(condition) CHECK((condition), "%s", #condition)

struct capture {
	unsigned char bytes[4096];
	size_t count;
	size_t limit;
};

static int output(void *context, const char *bytes, int count)
{
	struct capture *capture = context;
	size_t accepted = (size_t)count;
	if (accepted > capture->limit - capture->count)
		accepted = capture->limit - capture->count;
	memcpy(capture->bytes + capture->count, bytes, accepted);
	capture->count += accepted;
	return (int)accepted;
}

static int start(struct modem *modem) { (void)modem; return 0; }
static int stop(struct modem *modem) { (void)modem; return 0; }
static int control(struct modem *modem, unsigned cmd, unsigned long arg)
{ (void)modem; (void)cmd; (void)arg; return 0; }

static struct modem *create(struct capture *data, struct capture *commands)
{
	struct modem_driver driver = { "literal DTE", start, stop, control };
	struct modem *modem = modem_create(&driver, "literal");
	if (modem != NULL) {
		modem->data_output = output;
		modem->data_output_context = data;
		modem->control_output = output;
		modem->control_output_context = commands;
	}
	return modem;
}

static bool literal_and_partial(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL && modem->pty == -1 && modem->frontend_report == NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	const unsigned char literal[] = { 0, '+', '+', '+', '\r', 'A', 'T', '\r', 0xff };
	unsigned char received[4096], input[4096];
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == (int)sizeof(literal));
	EXPECT(modem->escape_count == 0 && modem->at_count == 0 && modem->event_scheduled == 0);
	EXPECT(modem->get_chars(modem, (char *)received, sizeof(received)) == (int)sizeof(literal));
	EXPECT(memcmp(received, literal, sizeof(literal)) == 0 && commands.count == 0);
	for (size_t index = 0; index < sizeof(input); ++index)
		input[index] = (unsigned char)index;
	EXPECT(modem_data_write(modem, input, sizeof(input)) == 4096);
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == 0);
	memset(input, 0x55, sizeof(input));
	EXPECT(modem->get_chars(modem, (char *)received, 17) == 17);
	EXPECT(modem_data_write(modem, literal, sizeof(literal)) == (int)sizeof(literal));
	EXPECT(modem->get_chars(modem, (char *)received + 17, 4096 - 17) == 4096 - 17);
	for (size_t index = 0; index < sizeof(received); ++index)
		EXPECT(received[index] == (unsigned char)index);
	EXPECT(modem->get_chars(modem, (char *)received, sizeof(received)) == (int)sizeof(literal));
	EXPECT(memcmp(received, literal, sizeof(literal)) == 0);
	modem_delete(modem);
	return true;
}

static bool command_preserves_receive(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	EXPECT(modem_data_write(modem, "x", 1) == 1);
	EXPECT(modem_set_dte_command(modem, true) == -1);
	char scratch;
	EXPECT(modem->get_chars(modem, &scratch, 1) == 1);
	unsigned carrier = modem->modem_info;
	void (*process)(struct modem *, void *, void *, int) = modem->process;
	EXPECT(modem_set_dte_command(modem, true) == 0);
	EXPECT(modem->command && modem->put_chars != NULL && modem->get_chars != NULL);
	EXPECT(modem_data_write(modem, "AT\r", 3) == -1);
	EXPECT(modem->put_chars(modem, "incoming\0+++", 12) == 12);
	EXPECT(data.count == 12 && commands.count == 0 && modem->modem_info == carrier);
	EXPECT(modem_set_dte_command(modem, false) == 0);
	EXPECT(!modem->command && modem->process == process && modem->retrain_pending == 0);
	modem_delete(modem);
	return true;
}

static bool raw_overflow_is_reported(void)
{
	struct capture data = { .limit = 0 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem->cfg.ec = modem->cfg.comp = 0;
	modem_update_status(modem, STATUS_EC_LINK);
	modem_async_start(modem);
	EXPECT(modem_data_write(modem, "x", 1) == 1);
	unsigned char bits[16];
	EXPECT(modem_async_get_bits(modem, 1, bits, 16) == 16);
	EXPECT(modem_async_put_bits(modem, 1, bits, 16) == 16);
	EXPECT(modem->dte_overflow && modem->disconnect_local);
	EXPECT(strcmp(modem->disconnect_reason, "raw DTE receive overflow") == 0);
	modem_delete(modem);
	return true;
}

static bool at_replies_are_separate(void)
{
	struct capture data = { .limit = 4096 }, commands = { .limit = 4096 };
	struct modem *modem = create(&data, &commands);
	EXPECT(modem != NULL);
	modem_frontend_init(modem);
	EXPECT(modem_write(modem, "AT\r", 3) == 3);
	EXPECT(commands.count != 0 && data.count == 0 && modem->xmit.count == 0);
	modem_delete(modem);
	return true;
}

int main(void)
{
	return literal_and_partial() && command_preserves_receive() &&
		raw_overflow_is_reported() && at_replies_are_separate() ? 0 : 1;
}
