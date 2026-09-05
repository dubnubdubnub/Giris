#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
"""
Does this ADC leave its sampling capacitor holding the previous conversion?

Same channel, same mux bank, same everything — only the channel converted
immediately BEFORE it changes. The VBUS divider sits ~1100 counts above a
quiet sensor, so if charge redistribution is real the victim moves by
~0.84% of that step (~9 LSB). If the sample cap is reset per conversion,
nothing moves.
"""
import statistics as st, struct, sys, time
import hid

VID, PID, RPT = 0x1209, 0x6415, 64
CMD_SNAPSHOT, CMD_SEQ_SET = 0x03, 0x08
RSP_SNAPSHOT, RSP_SEQ = 0x83, 0x88

d = hid.device(); d.open(VID, PID); d.set_nonblocking(0)

def send(cmd, *a):
    b = [0]*RPT; b[0], b[1] = cmd, 1
    for i, v in enumerate(a): b[2+i] = v
    d.write(bytes([0]+b))

def wait(msg, tmo=2.0):
    end = time.time()+tmo
    while time.time() < end:
        r = d.read(RPT, timeout_ms=200)
        if r and r[0] == msg: return bytes(r)
    return None

def set_seq(order):
    send(CMD_SEQ_SET, *order)
    r = wait(RSP_SEQ)
    return list(r[4:8]) if r else None

def sample(n=400):
    """mean of each of the 8 raw DMA slots"""
    acc = [[] for _ in range(8)]
    for _ in range(n):
        send(CMD_SNAPSHOT)
        r = wait(RSP_SNAPSHOT, 0.5)
        if not r: continue
        for s in range(8):
            acc[s].append(struct.unpack_from("<H", r, 8+2*s)[0])
    return [st.mean(a) if a else 0 for a in acc], [st.pstdev(a) if len(a)>1 else 0 for a in acc]

# rank1..4 as ADC channel numbers. Bank A slots are ranks 1-4, bank B are 5-8.
ORDERS = {
    "baseline  ch3,ch2,ch1,ch0": [3,2,1,0],   # ch1 preceded by ch2 (a sensor, ~2060)
    "VBUS-first ch0,ch3,ch2,ch1": [0,3,2,1],  # ch1 preceded by ch2 still... see note
    "VBUS-before-ch1 ch3,ch2,ch0,ch1": [3,2,0,1],  # ch1 now preceded by ch0 = VBUS (~3166)
}

print("channel -> what it is:  ch3=key0/key1  ch2=key2/key3  ch1=key4/key5  ch0=VBUS/unused\n")
res = {}
for name, order in ORDERS.items():
    got = set_seq(order)
    time.sleep(0.4)
    m, sd = sample()
    res[name] = (order, m, sd)
    # find which slot holds ch1 in bank A
    slot = order.index(1)
    print(f"{name}")
    print(f"   applied {got}   ch1 lands in bank-A slot {slot}")
    print(f"   slots: " + " ".join(f"{v:7.2f}" for v in m))
    print(f"   ch1 (magnet-free key4) = {m[slot]:8.3f}  sd {sd[slot]:.3f}\n")

set_seq([3,2,1,0])
base_order, base_m, _ = res["baseline  ch3,ch2,ch1,ch0"]
test_order, test_m, _ = res["VBUS-before-ch1 ch3,ch2,ch0,ch1"]
b = base_m[base_order.index(1)]; t = test_m[test_order.index(1)]
print("="*64)
print(f"ch1 with a sensor (~2060) before it : {b:8.3f}")
print(f"ch1 with VBUS   (~3166) before it   : {t:8.3f}")
print(f"difference                          : {t-b:+8.3f} LSB")
print(f"charge-sharing model predicts       : {0.0084*(3166-2060):+8.2f} LSB")
print("\nVERDICT:", "charge sharing IS real" if abs(t-b) > 3 else
      "no measurable charge sharing - the sample cap is reset per conversion")
d.close()
