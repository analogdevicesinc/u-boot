/*
 * Copyright (c) 2022, Analog Devices Inc.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <tee.h>
#include <vsprintf.h>
#include <dm/util.h>
#include <example_early_ta.h>

static u32 tee_session = 0;
static struct udevice *tee_device = NULL;

int do_tee_example_early_open(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct tee_optee_ta_uuid uuid = TA_EXAMPLE_EARLY_UUID;
	struct tee_open_session_arg open_args = { 0 };
	int rc;

	if (tee_session) {
		printf("Session is already open, please run 'tee_example_early close'\n");
		return CMD_RET_FAILURE;
	}

	if (tee_device == NULL) {
		tee_device = tee_find_device(tee_device, NULL, NULL, NULL);
		if (tee_device == NULL) {
			printf("Failed to find TEE device\n");
			return CMD_RET_FAILURE;
		}
	}

	tee_optee_ta_uuid_to_octets(open_args.uuid, &uuid);
	rc = tee_open_session(tee_device, &open_args, 0, NULL);
	if (rc < 0 || open_args.ret != TEE_SUCCESS) {
		printf("Failed to open session\n");
		return CMD_RET_FAILURE;
	}

	tee_session = open_args.session;
	return CMD_RET_SUCCESS;
}

int do_tee_example_early_close(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	tee_close_session(tee_device, tee_session);
	tee_session = 0;
	return CMD_RET_SUCCESS;
}

int do_tee_example_early_dummy(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct tee_invoke_arg invoke_args = { 0 };
	struct tee_param params[1] = { 0 };
	unsigned long val;
	int status;

	if (!tee_session) {
		printf("Session is not open, please run 'tee_example_early open'\n");
		return CMD_RET_FAILURE;
	}

	if (argc != 2)
		return CMD_RET_USAGE;

	val = hextoul(argv[1], NULL);

	invoke_args.func = TA_EXAMPLE_EARLY_CMD_DUMMY;
	invoke_args.session = tee_session;

	params[0].attr = TEE_PARAM_ATTR_TYPE_VALUE_INOUT;
	params[0].u.value.a = val;

	status = tee_invoke_func(tee_device, &invoke_args, 1, params);

	if (status < 0) {
		printf("Failed to invoke TEE command\n");
		return CMD_RET_FAILURE;
	}

	printf("Success: %llu\n", params[0].u.value.a);

	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_sub[] = {
	U_BOOT_CMD_MKENT(
		open,  1, 0, do_tee_example_early_open,	 "", ""
		),
	U_BOOT_CMD_MKENT(
		close, 1, 0, do_tee_example_early_close, "", ""
		),
	U_BOOT_CMD_MKENT(
		dummy, 2, 0, do_tee_example_early_dummy, "", ""
		),
};

int do_tee_example_early(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cmd_tbl *cp;

	cp = find_cmd_tbl(argv[1], cmd_sub, ARRAY_SIZE(cmd_sub));

	argc--;
	argv++;

	if (!cp || argc > cp->maxargs)
		return CMD_RET_USAGE;

	if (flag == CMD_FLAG_REPEAT)
		return CMD_RET_FAILURE;

	return cp->cmd(cmdtp, flag, argc, argv);
}

U_BOOT_CMD(
	/* command, maxargs, repeatable, handler_fn */
	tee_example_early, CONFIG_SYS_MAXARGS, 0, do_tee_example_early,
	/* brief help text */
	"Invoke commands of the 'example_early' TA.",
	/* full usage text */
	/* (the cmd is ommitted from the first line, u-boot inserts it) */
	"open - Open a session to the 'example_early' TA.\n"
	"tee_example_early close - Close the current 'example_early' TA session.\n"
	"tee_example_early dummy <num> - Invoke the TA 'dummy' command with <num>.\n"
	);
