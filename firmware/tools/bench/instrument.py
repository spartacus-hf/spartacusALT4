#!/usr/bin/env python3
"""Add BENCH-ONLY cycle counters to a copy of an alt2 tree (never to the
checkout, never committed; build_instrumented.sh runs it on a scratch copy).

bench_cyc[2i] = cycles net of nested counted contexts, [2i+1] = calls:
0 DMA1c1 (RF)  1 DMA1c2  2 DMA1c3 (LCD)  3 USB  4 SysTick
5 PendSV receive worker  6 PendSV render worker  7 DMA2c1 (ADC)
--probe FILE 'CALL;' SLOT (repeatable) also counts one call statement, e.g.
  --probe Core/Src/mod_tx_pipeline.c 'h12p_analytic_chunk(re, im, n, sideband_sign);' 10
cyc.py reads the layout.

usage: instrument.py ALT2_TREE [--probe FILE CALL SLOT]...
"""
import argparse
import os


def rd(p):
    with open(p, newline="") as f:
        return f.read()


def wr(p, s):
    with open(p, "w", newline="") as f:
        f.write(s)


def sub(s, old, new, p):
    if s.count(old) != 1:
        raise SystemExit(f"{p}: expected one {old[:60]!r}, found {s.count(old)} -- update instrument.py")
    return s.replace(old, new)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--probe", nargs=3, action="append", default=[], metavar=("FILE", "CALL", "SLOT"))
    a = ap.parse_args()
    root = a.root

    p = os.path.join(root, "Core/Src/stm32g4xx_it.c")
    s = rd(p)
    nl = "\r\n" if "\r\n" in s else "\n"
    block = nl.join(["", "/* BENCH-ONLY cycle accounting (not for commit). */",
                     "volatile uint32_t bench_cyc[24];", "volatile uint32_t bench_nest;",
                     "static inline uint32_t bench_t0(uint32_t *n0) { *n0 = bench_nest; return DWT->CYCCNT; }",
                     "void bench_t1(uint32_t i, uint32_t t0, uint32_t n0)", "{",
                     "  uint32_t pm = __get_PRIMASK(); __disable_irq();", "  uint32_t wall = DWT->CYCCNT - t0;",
                     "  bench_cyc[2U * i] += wall - (bench_nest - n0);", "  bench_cyc[2U * i + 1U]++;",
                     "  bench_nest = n0 + wall;", "  __set_PRIMASK(pm);", "}",
                     "uint32_t bench_begin(uint32_t *n0) { return bench_t0(n0); }",
                     "#define BENCH(i, stmt) do { uint32_t n0_, t0_ = bench_t0(&n0_); stmt; bench_t1((i), t0_, n0_); } while (0)",
                     ""])
    s = sub(s, '#include "modulator.h"' + nl, '#include "modulator.h"' + nl + block, p)
    s = sub(s, "  RADIO_AudioWorker();" + nl + "  MOD_RealtimeRenderWorker();",
            "  BENCH(5, RADIO_AudioWorker());" + nl + "  BENCH(6, MOD_RealtimeRenderWorker());", p)
    s = sub(s, "void SysTick_Handler(void)" + nl + "{" + nl,
            "void SysTick_Handler(void)" + nl + "{" + nl + "  uint32_t sn0_, st0_ = bench_t0(&sn0_);" + nl, p)
    s = sub(s, "  RADIO_PdTick();" + nl, "  RADIO_PdTick();" + nl + "  bench_t1(4U, st0_, sn0_);" + nl, p)
    s = sub(s, "  mod_dma_irq_channel1();", "  BENCH(0, mod_dma_irq_channel1());", p)
    s = sub(s, "  mod_dma_irq_channel2();", "  BENCH(1, mod_dma_irq_channel2());", p)
    if "  HAL_DMA_IRQHandler(&hdma_spi4_tx);" in s:
        s = sub(s, "  HAL_DMA_IRQHandler(&hdma_spi4_tx);", "  BENCH(2, HAL_DMA_IRQHandler(&hdma_spi4_tx));", p)
    else:
        s = sub(s, "  LCDUI_DmaIrq();", "  BENCH(2, LCDUI_DmaIrq());", p)
    s = sub(s, "void USB_LP_IRQHandler(void)" + nl + "{" + nl,
            "void USB_LP_IRQHandler(void)" + nl + "{" + nl + "  uint32_t bn0_, bt0_ = bench_t0(&bn0_);" + nl, p)
    s = sub(s, "  HAL_PCD_IRQHandler(&hpcd_USB_FS);" + nl,
            "  HAL_PCD_IRQHandler(&hpcd_USB_FS);" + nl + "  bench_t1(3U, bt0_, bn0_);" + nl, p)
    wr(p, s)

    p = os.path.join(root, "Core/Src/radio_analog.c")
    s = rd(p)
    nl = "\r\n" if "\r\n" in s else "\n"
    s = sub(s, "void DMA2_Channel1_IRQHandler(void)" + nl, "static void adc_dma_irq(void)" + nl, p)
    s += nl.join(["", "/* BENCH-ONLY wrapper (not for commit). */",
                  "extern void bench_t1(uint32_t i, uint32_t t0, uint32_t n0);",
                  "extern uint32_t bench_begin(uint32_t *n0);", "void DMA2_Channel1_IRQHandler(void)", "{",
                  "    uint32_t n0, t0 = bench_begin(&n0);", "    adc_dma_irq();", "    bench_t1(7U, t0, n0);", "}", ""])
    wr(p, s)

    for rel, call, slot in a.probe:
        if not 8 <= int(slot) <= 11:
            raise SystemExit("probe slots are 8-11")
        p = os.path.join(root, rel)
        s = rd(p)
        wrapped = ("{ extern void bench_t1(uint32_t i, uint32_t t0, uint32_t n0); "
                   "extern uint32_t bench_begin(uint32_t *n0); "
                   f"uint32_t bn0_, bt0_ = bench_begin(&bn0_); {call} bench_t1({int(slot)}U, bt0_, bn0_); }}")
        wr(p, sub(s, call, wrapped, p))
    print("instrumented", root, "probes:", [(c, s) for _, c, s in a.probe])


if __name__ == "__main__":
    main()
