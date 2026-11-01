// Pre-analysis setup for an ESP32-C3 (ESP-IDF v4.4) app ELF produced by img2elf.py.
// Adds ROM / peripheral / BSS memory blocks, ESP32-C3 ROM symbols from ESP-IDF *.ld files
// and peripheral base labels.  Args: <dir containing esp32c3.rom*.ld>
//@category ESP32
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class Esp32c3Setup extends GhidraScript {
    void block(String name, long start, long len, boolean x, boolean w, boolean vol) throws Exception {
        Memory mem = currentProgram.getMemory();
        Address a = toAddr(start);
        if (mem.getBlock(a) != null) return;
        MemoryBlock b = mem.createUninitializedBlock(name, a, len, false);
        b.setRead(true); b.setWrite(w); b.setExecute(x); b.setVolatile(vol);
    }
    void gapFill(String name, long start, long end) throws Exception {
        // create uninitialized blocks over the parts of [start,end) not already mapped
        Memory mem = currentProgram.getMemory();
        long p = start;
        while (p < end) {
            MemoryBlock b = mem.getBlock(toAddr(p));
            if (b != null) { p = b.getEnd().getOffset() + 1; continue; }
            long q = p;
            while (q < end && mem.getBlock(toAddr(q)) == null) q += 4;
            block(name + String.format("_%08x", p), p, q - p, false, true, false);
            p = q;
        }
    }
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        block("irom_rom", 0x40000000L, 0x60000L, true, false, false);
        block("drom_rom", 0x3ff00000L, 0x20000L, false, false, false);
        block("peripherals", 0x60000000L, 0xD1000L, false, true, true);
        block("rtc_fast", 0x50000000L, 0x2000L, false, true, false);
        gapFill("dram", 0x3fc80000L, 0x3fce0000L);
        gapFill("iram", 0x40380000L, 0x403e0000L);
        // ROM symbols
        Pattern p = Pattern.compile("^\\s*(?:PROVIDE\\s*\\(\\s*)?([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(0x[0-9a-fA-F]+)");
        Pattern alias = Pattern.compile("^\\s*PROVIDE\\s*\\(\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*\\)");
        Map<String, Long> syms = new LinkedHashMap<>();
        List<String[]> aliases = new ArrayList<>();
        int n = 0;
        if (args.length > 0) {
            File dir = new File(args[0]);
            for (File f : dir.listFiles()) {
                if (!f.getName().endsWith(".ld") || f.getName().contains("eco7")) continue;
                for (String line : Files.readAllLines(f.toPath())) {
                    Matcher m = p.matcher(line);
                    if (m.find()) { syms.put(m.group(1), Long.decode(m.group(2))); continue; }
                    m = alias.matcher(line);
                    if (m.find()) aliases.add(new String[]{m.group(1), m.group(2)});
                }
            }
        }
        for (String[] al : aliases) if (syms.containsKey(al[1]) && !syms.containsKey(al[0])) syms.put(al[0], syms.get(al[1]));
        SymbolTable st = currentProgram.getSymbolTable();
        for (Map.Entry<String, Long> e : syms.entrySet()) {
            Address a = toAddr(e.getValue());
            if (currentProgram.getMemory().getBlock(a) == null) continue;
            st.createLabel(a, e.getKey(), SourceType.IMPORTED);
            long v = e.getValue();
            if (v >= 0x40000000L && v < 0x40060000L && getFunctionAt(a) == null) {
                try { createFunction(a, e.getKey()); } catch (Exception ex) {}
            }
            n++;
        }
        // ESP32-C3 peripheral bases (TRM / soc/esp32c3/include/soc/soc.h)
        String[][] per = {
            {"UART0","0x60000000"},{"SPI1","0x60002000"},{"SPI0","0x60003000"},{"GPIO","0x60004000"},
            {"TIMG_unused","0x60007000"},{"RTC_CNTL","0x60008000"},{"IO_MUX","0x60009000"},
            {"UART1","0x60010000"},{"I2C0","0x60013000"},{"UHCI0","0x60014000"},{"RMT","0x60016000"},
            {"LEDC","0x60019000"},{"EFUSE","0x60008800"},{"TIMG0","0x6001F000"},{"TIMG1","0x60020000"},
            {"SYSTIMER","0x60023000"},{"SPI2","0x60024000"},{"APB_CTRL","0x60026000"},{"TWAI","0x6002B000"},
            {"I2S0","0x6002D000"},{"APB_SARADC","0x60040000"},{"USB_SERIAL_JTAG","0x60043000"},
            {"GDMA","0x6003F000"},{"SYSTEM","0x600C0000"},{"SENSITIVE","0x600C1000"},{"INTERRUPT_CORE0","0x600C2000"},
            {"EXTMEM","0x600C4000"},{"RTC_I2C","0x6000E000"},{"SYSCON","0x60026000"}};
        for (String[] pp : per) st.createLabel(toAddr(Long.decode(pp[1])), pp[0], SourceType.IMPORTED);
        println("Esp32c3Setup: " + n + " ROM symbols");
    }
}
