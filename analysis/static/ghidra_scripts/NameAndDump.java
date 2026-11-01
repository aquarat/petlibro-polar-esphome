// Post-analysis: (1) name functions from ESP-IDF __func__ strings used in ESP_LOGx / assert calls,
// (2) write string->function xrefs, (3) decompile every function into one C file.
// Args: <output dir>
//@category ESP32
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class NameAndDump extends GhidraScript {
    String readCString(Address a, int max) {
        try {
            StringBuilder sb = new StringBuilder();
            Memory m = currentProgram.getMemory();
            for (int i = 0; i < max; i++) {
                byte b = m.getByte(a.add(i));
                if (b == 0) return sb.toString();
                if (b < 0x09 || (b & 0xff) > 0x7e) return null;
                sb.append((char) b);
            }
        } catch (Exception e) { }
        return null;
    }
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        new File(out).mkdirs();
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        // collect references from code into DROM string-looking data
        Map<Address, Set<Function>> strRefs = new TreeMap<>();
        Map<Address, String> strVal = new HashMap<>();
        AddressSetView drom = new AddressSet(toAddr(0x3c0f0020L), toAddr(0x3c11be20L));
        AddressIterator it = rm.getReferenceDestinationIterator(drom, true);
        while (it.hasNext()) {
            Address d = it.next();
            String s = readCString(d, 512);
            if (s == null || s.length() < 2) continue;
            for (Reference r : rm.getReferencesTo(d)) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                strRefs.computeIfAbsent(d, k -> new LinkedHashSet<>()).add(f);
                strVal.put(d, s);
            }
        }
        // naming pass
        PrintWriter nl = new PrintWriter(new FileWriter(out + "/func_names.txt"));
        Map<Function, List<String>> cand = new HashMap<>();
        for (Map.Entry<Address, Set<Function>> e : strRefs.entrySet()) {
            String s = strVal.get(e.getKey());
            if (!s.matches("[A-Za-z_][A-Za-z0-9_]{3,63}")) continue;
            if (e.getValue().size() > 2) continue; // tags are shared by many functions
            for (Function f : e.getValue()) cand.computeIfAbsent(f, k -> new ArrayList<>()).add(s);
        }
        int named = 0;
        for (Map.Entry<Function, List<String>> e : cand.entrySet()) {
            Function f = e.getKey();
            List<String> c = e.getValue();
            // prefer candidates that look like function names (contain '_' or CamelCase and are not tags)
            String pick = null;
            if (c.size() == 1) pick = c.get(0);
            nl.println(f.getEntryPoint() + " " + f.getName() + " -> " + c + (pick != null ? " NAMED" : ""));
            if (pick != null && f.getName().startsWith("FUN_")) {
                try { f.setName(pick, SourceType.ANALYSIS); named++; } catch (Exception ex) {
                    try { f.setName(pick + "_" + f.getEntryPoint(), SourceType.ANALYSIS); named++; } catch (Exception ex2) {}
                }
            }
        }
        nl.close();
        println("named " + named);
        // string xref listing
        PrintWriter sx = new PrintWriter(new FileWriter(out + "/string_xrefs.txt"));
        for (Map.Entry<Address, Set<Function>> e : strRefs.entrySet()) {
            StringBuilder sb = new StringBuilder();
            for (Function f : e.getValue()) sb.append(f.getName()).append("@").append(f.getEntryPoint()).append(" ");
            sx.println(e.getKey() + "\t" + strVal.get(e.getKey()).replace("\n", "\\n").replace("\u001b", "") + "\t" + sb);
        }
        sx.close();
        // function list
        PrintWriter fl = new PrintWriter(new FileWriter(out + "/functions.txt"));
        for (Function f : fm.getFunctions(true)) fl.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName());
        fl.close();
        // decompile
        DecompInterface di = new DecompInterface();
        DecompileOptions opt = new DecompileOptions();
        di.setOptions(opt);
        di.openProgram(currentProgram);
        PrintWriter c = new PrintWriter(new FileWriter(out + "/decomp_all.c"));
        int n = 0;
        for (Function f : fm.getFunctions(true)) {
            long ep = f.getEntryPoint().getOffset();
            if (ep >= 0x40000000L && ep < 0x40060000L) continue; // ROM
            if (monitor.isCancelled()) break;
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            c.println("\n// ===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            if (r != null && r.decompileCompleted()) c.println(r.getDecompiledFunction().getC());
            else c.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
            n++;
        }
        c.close();
        di.dispose();
        println("decompiled " + n);
    }
}
