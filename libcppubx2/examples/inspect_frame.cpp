// SPDX-License-Identifier: GPL-3.0-only
#include <cppubx2/ubx.hpp>
#include <iostream>

int main()
{
    // An ACK-ACK frame body: class, ID, length, payload, CK_A, CK_B.
    // ubx_frame takes bytes AFTER the B5 62 sync marker.
    const UBX::ubx_buf_t bytes{5, 1, 2, 0, 1, 2, 11, 47};
    const UBX::ubx_frame frame(bytes);
    if(!frame.valid) return 1;
    std::cout << UBX::ubx_msg_name(frame.class_id, frame.msg_id) << '\n';
    return 0;
}
