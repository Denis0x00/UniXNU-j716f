/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* XPC bootstrap adapter for TB-J716F iPadOS 17.7.11 (21H461) QCA6390.
 * HCI, pairing, keys and profiles stay in stock bluetoothd. */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <os/log.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>
#include <xpc/xpc.h>

/* Present in iPadOS BlueTool imports; the public iphoneos SDK hides listeners. */
extern xpc_connection_t qbt_xpc_listener(const char *, dispatch_queue_t, uint64_t)
    __asm__("_xpc_connection_create_mach_service");

static os_log_t logger;

static int power_on(void)
{
    int result = EIO;
    size_t length = sizeof(result);
    /* A denied request is not a controller failure: UART.SK connect also starts it. */
    if (sysctlbyname("kern.qbt_power_on", &result, &length, NULL, 0))
        return errno == EPERM ? 0 : errno;
    return length == sizeof(result) ? result : EIO;
}

static int state(void)
{
    int result = EIO;
    size_t length = sizeof(result);
    if (sysctlbyname("kern.qbt_boot", &result, &length, NULL, 0))
        return errno;
    return length == sizeof(result) ? result : EIO;
}

static void handle(xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply) return;
    const char *id = xpc_dictionary_get_string(message, "kMsgId");
    int error = ENOTSUP;
    if (id && !strcmp(id, "RunBuiltinScript")) {
        xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
        const char *script = (args && xpc_get_type(args) == XPC_TYPE_DICTIONARY)
                                 ? xpc_dictionary_get_string(args, "script")
                                 : NULL;
        if (script && (!strcmp(script, "boot") || !strcmp(script, "init")))
            error = power_on();
        else if (script && !strcmp(script, "deepsleep")) {
            error = state();
            if (error == ENETDOWN || error == EPERM) error = 0;
        } else {
            error = EINVAL;
        }
        os_log(logger, "script %{public}s result %d", script ? script : "missing", error);
    }
    xpc_dictionary_set_int64(reply, "result", error ? 1 : 0);
    if (!error) {
        char firmware[64] = {0};
        size_t length = sizeof(firmware) - 1;
        (void)sysctlbyname("kern.qbt_firmware", firmware, &length, NULL, 0);
        /* 29 = Qualcomm Technologies International, Ltd. (Bluetooth SIG Company ID). */
        xpc_dictionary_set_int64(reply, "manufacturer", 29);
        xpc_dictionary_set_string(reply, "firmware", firmware);
    }
    if (id && !strcmp(id, "RunCommand"))
        xpc_dictionary_set_string(reply, "output", "Unsupported controller command");
    xpc_connection_t remote = xpc_dictionary_get_remote_connection(message);
    if (remote) xpc_connection_send_message(remote, reply);
    xpc_release(reply);
}

int main(void)
{
    logger = os_log_create("org.unixnu.bluetooth", "BlueTool");
    dispatch_queue_t queue = dispatch_queue_create("org.unixnu.bluetooth.bootstrap", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = qbt_xpc_listener("com.apple.BlueTool", queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 1;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t event) { handle(event); });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
