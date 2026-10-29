// Modern Linux BlueZ pairing implementation:
// Copyright (c) 2026 ichikand. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// -- includes -----
#include "BluetoothQueries.h"
#include "ServerLog.h"
#include "ServerUtility.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <algorithm>
#include <cctype>

//-- Queries -----
bool bluetooth_get_host_address(std::string &out_address)
{
    bool bSuccess = false;
    int dev_id = hci_get_route(NULL);

    if (dev_id < 0)
    {
        int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
        if (sk >= 0)
        {
            struct hci_dev_list_req *dl = (struct hci_dev_list_req *)malloc(
                HCI_MAX_DEV * sizeof(struct hci_dev_req) + sizeof(struct hci_dev_list_req));
            if (dl)
            {
                dl->dev_num = HCI_MAX_DEV;
                if (ioctl(sk, HCIGETDEVLIST, (void *)dl) >= 0)
                {
                    for (int i = 0; i < dl->dev_num; ++i)
                    {
                        if (dl->dev_req[i].dev_id >= 0)
                        {
                            dev_id = dl->dev_req[i].dev_id;
                            break;
                        }
                    }
                }
                free(dl);
            }
            close(sk);
        }
    }

    if (dev_id >= 0)
    {
        bdaddr_t bdaddr;
        if (hci_devba(dev_id, &bdaddr) == 0)
        {
            char addr_str[18] = {0};
            ba2str(&bdaddr, addr_str);
            out_address = std::string(addr_str);
            std::transform(out_address.begin(), out_address.end(), out_address.begin(),
                [](unsigned char c) { return std::tolower(c); });
            bSuccess = true;
        }
    }

    return bSuccess;
}