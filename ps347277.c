#include <minix/drivers.h>
#include <minix/chardriver.h>
#include <stdio.h>
#include <stdlib.h>
#include <minix/ds.h>

#define MAX_CLIPBOARD_ENTRIES 100
#define STATE_WRITE 100
#define DEBUG_CTL 1337

#define MAX_LENGTH_ENTRY 200

static int clipboard_open(devminor_t minor, int access, endpoint_t user_endpt);
static int clipboard_close(devminor_t minor);
static ssize_t clipboard_read(devminor_t minor, u64_t position, endpoint_t endpt,
                          cp_grant_id_t grant, size_t size, int flags, cdev_id_t id);
static ssize_t clipboard_write(devminor_t UNUSED(minor), u64_t position,
endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
    cdev_id_t UNUSED(id));
static int clipboard_ioctl(devminor_t UNUSED(minor), unsigned long request, endpoint_t endpt,
	cp_grant_id_t grant, int UNUSED(flags), endpoint_t UNUSED(user_endpt),
	cdev_id_t UNUSED(id));

/* SEF functions and variables. */
static void sef_local_startup(void);
static int sef_cb_init(int type, sef_init_info_t *info);
static int sef_cb_lu_state_save(int);
static int lu_state_restore(void);

static struct chardriver clipboard_tab =
        {
                .cdr_open	= clipboard_open,
                .cdr_close	= clipboard_close,
                .cdr_read	= clipboard_read,
                .cdr_write      = clipboard_write,
                .cdr_ioctl      = clipboard_ioctl
        };

static int number_of_active_entries;
static int current_index;
static char** clipboard;
static int* clipboard_lengths;

static int clipboard_open(devminor_t UNUSED(minor), int UNUSED(access),
        endpoint_t UNUSED(user_endpt))
{
        return OK;
}

static int clipboard_close(devminor_t UNUSED(minor))
{
        return OK;
}

static ssize_t clipboard_write(devminor_t UNUSED(minor), u64_t position,
endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
    cdev_id_t UNUSED(id))
{
        int ret;

        if (number_of_active_entries >= MAX_CLIPBOARD_ENTRIES) {
                return -1;
        }

        if (size > MAX_LENGTH_ENTRY) {
                return -1; // otherwise we wouldnt be able to retrieve entries on service update
        }

        clipboard[current_index] = malloc(sizeof(char) * size);

        if (clipboard[current_index] == NULL) {
                return -1; // malloc failed to allocate for this size;
        }

        if ((ret = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) clipboard[current_index], size)) != OK) {
                free(clipboard[current_index]);
                clipboard[current_index] = NULL;
                return ret;
        }

        number_of_active_entries = number_of_active_entries + 1;

        clipboard_lengths[current_index] = size;

        return size;
}

static ssize_t clipboard_read(devminor_t UNUSED(minor), u64_t position,
endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
        cdev_id_t UNUSED(id))
{
        if (number_of_active_entries <= 0) {
                return -1;
        }

        int ret;

        if (size < clipboard_lengths[current_index]) {
                return -1;
        }

        if (size > clipboard_lengths[current_index]) {
                size = clipboard_lengths[current_index];
        }

        if ((ret = sys_safecopyto(endpt, grant, 0, (vir_bytes) clipboard[current_index], size)) != OK) {
                return ret;
        }

        free(clipboard[current_index]);
        clipboard[current_index] = NULL; // no use after free;
        clipboard_lengths[current_index] = 0;

        number_of_active_entries = number_of_active_entries - 1;

        return size;
}

int find_first_free_index() {
        if (number_of_active_entries >= MAX_CLIPBOARD_ENTRIES) {
                return -1;
        }

        for (int i = 0; i < MAX_CLIPBOARD_ENTRIES; i++) {
                if (clipboard[i] == NULL) {
                        return i;
                }
        }

        return -1;
}

static void debug_print() {
        printf("CLIPBOARD DEBUG\n");
        printf("active items: %d\n", number_of_active_entries);
        for (int i = 0; i < MAX_CLIPBOARD_ENTRIES; i++) {
                if (clipboard[i] == NULL) {
                        printf("id: %d, length: %d, NULL!\n", i, clipboard_lengths[i]);
                }
                else {
                        printf("id: %d, length: %d, text: %.*s\n", i, clipboard_lengths[i], clipboard_lengths[i], clipboard[i]);
                }
        }
}

static int clipboard_ioctl(devminor_t UNUSED(minor), unsigned long request, endpoint_t endpt,
	cp_grant_id_t grant, int UNUSED(flags), endpoint_t UNUSED(user_endpt),
	cdev_id_t UNUSED(id))
{
	int r;

	switch (request) {
        case DEBUG_CTL:
                debug_print();
                return DEBUG_CTL;
                break;
	case STATE_WRITE:
                current_index = find_first_free_index();
                r = current_index;
		break;
	default:
                if (request < MAX_CLIPBOARD_ENTRIES) {
                        current_index = request;
                        r = current_index;
                        break;
                }
		r = ENOTTY;
		break;
	}

	return r;
}

static int sef_cb_lu_state_save(int UNUSED(state)) {
        char buffer[20];
        char buffer2[2*MAX_LENGTH_ENTRY + 1];
        ds_publish_u32("pszulc_number_entries", number_of_active_entries, DSF_OVERWRITE);
        ds_publish_u32("pszulc_index", current_index, DSF_OVERWRITE);
        ds_publish_mem("pszulc_lengths", (char *) clipboard_lengths, (sizeof(int) / sizeof(char)) * MAX_CLIPBOARD_ENTRIES, DSF_OVERWRITE);

        // workaround for problem when trying to save 100 individual entries
        // ds runs out of memory
        // im forced to save them all
        for (int i = 0; i < MAX_CLIPBOARD_ENTRIES; i += 2) {
                if (clipboard_lengths[i] + clipboard_lengths[i+1] > 0) {
                        sprintf(buffer, "pszulc_clipboard_%d", i);
                        sprintf(buffer2, "%s%s", clipboard[i], clipboard[i+1]);
                        int r = ds_publish_mem(buffer, buffer2, clipboard_lengths[i] + clipboard_lengths[i+1], DSF_OVERWRITE);
                }
        }

        return OK;
}

static int lu_state_restore() {
        char buffer[50];
        char buffer2[2*MAX_LENGTH_ENTRY];
        u32_t entries;
        u32_t index;

        ds_retrieve_u32("pszulc_number_entries", &entries);
        ds_delete_u32("pszulc_number_entries");
        number_of_active_entries = (int) entries;

        ds_retrieve_u32("pszulc_index", &index);
        ds_delete_u32("pszulc_index");
        current_index = (int) index;

        unsigned int lengths_size = sizeof(int) / sizeof(char) * MAX_CLIPBOARD_ENTRIES;
        ds_retrieve_mem("pszulc_lengths", (char *) clipboard_lengths, &lengths_size);
        ds_delete_mem("pszulc_lengths");


        for (int i = 0; i < MAX_CLIPBOARD_ENTRIES; i += 2) {
                int length = clipboard_lengths[i] + clipboard_lengths[i+1];
                if (clipboard_lengths[i] + clipboard_lengths[i+1] > 0) {
                        sprintf(buffer, "pszulc_clipboard_%d", i);
                        ds_retrieve_mem(buffer, buffer2, &length);
                        ds_delete_mem(buffer);

                        if (clipboard_lengths[i] > 0) {
                                clipboard[i] = malloc(sizeof(char) * clipboard_lengths[i]);
                                memcpy(clipboard[i], buffer2, clipboard_lengths[i]);
                        }
                        if (clipboard_lengths[i+1] > 0) {
                                clipboard[i+1] = malloc(sizeof(char) * clipboard_lengths[i+1]);
                                memcpy(clipboard[i+1], buffer2 + clipboard_lengths[i], clipboard_lengths[i+1]);
                        }
                }
        }

        return OK;
}

static void sef_local_startup()
{
    /*
     * Register init callbacks. Use the same function for all event types
     */
    sef_setcb_init_fresh(sef_cb_init);
    sef_setcb_init_lu(sef_cb_init);
    sef_setcb_init_restart(sef_cb_init);

    /*
     * Register live update callbacks.
     */
    /* - Agree to update immediately when LU is requested in a valid state. */
    sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
    /* - Support live update starting from any standard state. */
    sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
    /* - Register a custom routine to save the state. */
    sef_setcb_lu_state_save(sef_cb_lu_state_save);

    /* Let SEF perform startup. */
    sef_startup();
}

static int sef_cb_init(int type, sef_init_info_t *UNUSED(info))
{
    int do_announce_driver = TRUE;

    number_of_active_entries = 0;
    current_index = 0;
    clipboard = malloc((sizeof(char *) * MAX_CLIPBOARD_ENTRIES));
    clipboard_lengths = malloc((sizeof(int) * MAX_CLIPBOARD_ENTRIES));

    switch(type) {
        case SEF_INIT_FRESH:
            break;

        case SEF_INIT_LU:
            /* Restore the state. */
            lu_state_restore();
            do_announce_driver = FALSE;
            break;

        case SEF_INIT_RESTART:
            break;
    }

    /* Announce we are up when necessary. */
    if (do_announce_driver) {
        chardriver_announce();
    }

    /* Initialization completed successfully. */
    return OK;
}

int main(void)
{
    /*
     * Perform initialization.
     */
    sef_local_startup();

    /*
     * Run the main loop.
     */
    chardriver_task(&clipboard_tab);
    return OK;
}

