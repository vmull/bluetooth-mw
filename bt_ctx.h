
struct bt_ctx {
	bdaddr_t local_mac;   /**< our MAC */
	bdaddr_t remote_mac;  /**< other end MAC */

	uint8_t pin_code[16];

	int dev_id;            /**< local bt device id */

	int local_port;        /**< port we use */
	int remote_port;       /**< port other end uses */

	int hci_fd;            /**< opened HCI device fd */
	uint16_t conn_handle;  /**< handle of established connection (HCI tests only) */

	int clock_offset;

	struct timespec test_timeout;     /**< timeout for test case */
};
