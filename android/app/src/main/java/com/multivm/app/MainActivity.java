package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import com.multivm.core.Architecture;
import com.multivm.core.DiskImages;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Edicao de uma VM, em cartoes por categoria. Os campos sao salvos ao sair da tela. */
public class MainActivity extends BaseActivity implements MediaListView.Host {

    private static final Architecture[] ARCHS = AppFiles.ARCHS;
    /** extra do Intent: id da VM a editar (VmSettings) */
    static final String EXTRA_VM_ID = "vmId";

    /* requestCodes do seletor de arquivos */
    private static final int REQ_KERNEL = 0, REQ_INITRD = 1, REQ_DTB = 2, REQ_MEDIA = 10, REQ_CONVERT = 100;

    private VmSettings settings;
    private Spinner arch, bootOrder;
    private EditText ram, cmdline, fbSize;
    private SeekBar ramBar;
    private RadioButton modeBios, modeKernel, sata, virtioCtrl;
    private CheckBox fbEnable;
    private View kernelGroup, biosGroup, ctrlGroup;
    private MediaListView[] lists;
    private Button start;
    private EditText title;
    private TextView info, ctrlHelp, chipArch, chipRam, chipMode, chipMedia, kernelName, initrdName;
    private String kernel, initrd;
    private final List<String> bootCodes = new ArrayList<>();
    private boolean starting, loading;
    /* som, rede e VNC */
    private Spinner sound, nic;
    private CheckBox mic, vnc, vncLan;
    private EditText vncPort, vncPassword;
    private LinearLayout fwdList;
    private TextView netHelp, vncHelp;
    private View vncGroup;
    private final List<String> forwards = new ArrayList<>();
    private ForwardList fwd;
    private static final int REQ_MIC = 200;
    private boolean micAsked;
    /* relogio, resolucao, parametros do kernel e opcoes ARM */
    private Spinner rtcMode, fbRes;
    private EditText rtcDate, rawAddr;
    private TextView archHelp, dtbName, ramHelp;
    private View dtbGroup;
    private LinearLayout quickParams;
    private String dtb;
    /** placas oferecidas para a arquitetura atual (indices globais de VmSettings) */
    private int[] nicIdx, sndIdx;
    private Boolean choicesX86;

    /** atalhos da linha de comando: parametro e rotulo */
    private static final String[][] QUICK = {{"root=/dev/vda", "raiz /dev/vda"}, {"root=/dev/vda1", "raiz /dev/vda1"},
            {"root=/dev/vda2", "raiz /dev/vda2"}, {"rootwait", "rootwait"}, {"rw", "rw"}, {"earlycon", "earlycon"},
            {"console=tty0", "console na tela"}, {"quiet", "quiet"}, {"loglevel=7", "loglevel=7"},
            {"init=/bin/sh", "init=/bin/sh"}};
    private final DiskCreator diskCreator = new DiskCreator(this);

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        settings = VmSettings.find(this, getIntent().getStringExtra(EXTRA_VM_ID));
        if (settings == null) { /* VM inexistente (ex.: excluida): volta para a lista */
            finish();
            return;
        }
        setContentView(R.layout.activity_main);

        arch = findViewById(R.id.arch);
        bootOrder = findViewById(R.id.bootOrder);
        ram = findViewById(R.id.ram);
        ramBar = findViewById(R.id.ramBar);
        ramHelp = findViewById(R.id.ramHelp);
        cmdline = findViewById(R.id.cmdline);
        fbSize = findViewById(R.id.fbSize);
        modeBios = findViewById(R.id.modeBios);
        modeKernel = findViewById(R.id.modeKernel);
        sata = findViewById(R.id.sata);
        virtioCtrl = findViewById(R.id.virtioCtrl);
        fbEnable = findViewById(R.id.fbEnable);
        kernelGroup = findViewById(R.id.kernelGroup);
        biosGroup = findViewById(R.id.biosGroup);
        ctrlGroup = findViewById(R.id.ctrlGroup);
        ctrlHelp = findViewById(R.id.ctrlHelp);
        start = findViewById(R.id.start);
        title = findViewById(R.id.title);
        info = findViewById(R.id.info);
        chipArch = findViewById(R.id.chipArch);
        chipRam = findViewById(R.id.chipRam);
        chipMode = findViewById(R.id.chipMode);
        chipMedia = findViewById(R.id.chipMedia);
        kernelName = findViewById(R.id.kernelName);
        initrdName = findViewById(R.id.initrdName);
        lists = new MediaListView[]{findViewById(R.id.mlDisks), findViewById(R.id.mlCds), findViewById(R.id.mlFloppies)};
        for (int k = 0; k < lists.length; k++) lists[k].setup(k, this);

        arch.setAdapter(spinnerAdapter(AppFiles.ARCH_NAMES));
        arch.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { updateMode(); }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        modeBios.setOnCheckedChangeListener((b, c) -> updateMode());
        modeKernel.setOnCheckedChangeListener((b, c) -> updateMode());
        sata.setOnCheckedChangeListener((b, c) -> updateMode());
        virtioCtrl.setOnCheckedChangeListener((b, c) -> updateMode());
        AppFiles.bindRam(ram, ramBar, this::updateChips);
        fbEnable.setOnCheckedChangeListener((b, c) -> updateFb());

        rtcMode = findViewById(R.id.rtcMode);
        rtcDate = findViewById(R.id.rtcDate);
        fbRes = findViewById(R.id.fbRes);
        rawAddr = findViewById(R.id.rawAddr);
        archHelp = findViewById(R.id.archHelp);
        dtbName = findViewById(R.id.dtbName);
        dtbGroup = findViewById(R.id.dtbGroup);
        quickParams = findViewById(R.id.quickParams);
        rtcMode.setAdapter(spinnerAdapter(VmSettings.RTC_NAMES));
        rtcMode.setOnItemSelectedListener(onSelect(() -> rtcDate.setVisibility(
                rtcMode.getSelectedItemPosition() == VmSettings.RTC_FIXED ? View.VISIBLE : View.GONE)));
        String[] res = Arrays.copyOf(AppSettings.RESOLUTIONS, AppSettings.RESOLUTIONS.length + 1);
        res[res.length - 1] = "Personalizada…";
        fbRes.setAdapter(spinnerAdapter(res));
        fbRes.setOnItemSelectedListener(onSelect(this::updateFb));
        findViewById(R.id.dtbPick).setOnClickListener(v -> pick(REQ_DTB, false));
        findViewById(R.id.dtbClear).setOnClickListener(v -> setDtb(null));
        cmdline.addTextChangedListener(new android.text.TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence t, int a, int b, int c) {}

            @Override
            public void onTextChanged(CharSequence t, int a, int b, int c) {}

            @Override
            public void afterTextChanged(android.text.Editable e) { renderQuickParams(); }
        });

        findViewById(R.id.kernelPick).setOnClickListener(v -> pick(REQ_KERNEL, false));
        findViewById(R.id.initrdPick).setOnClickListener(v -> pick(REQ_INITRD, false));
        findViewById(R.id.kernelClear).setOnClickListener(v -> setKernel(null));
        findViewById(R.id.initrdClear).setOnClickListener(v -> setInitrd(null));
        start.setOnClickListener(v -> onStartClicked());
        findViewById(R.id.back).setOnClickListener(v -> finish());

        sound = findViewById(R.id.sound);
        nic = findViewById(R.id.nic);
        mic = findViewById(R.id.mic);
        vnc = findViewById(R.id.vnc);
        vncLan = findViewById(R.id.vncLan);
        vncPort = findViewById(R.id.vncPort);
        vncPassword = findViewById(R.id.vncPassword);
        fwdList = findViewById(R.id.fwdList);
        netHelp = findViewById(R.id.netHelp);
        vncHelp = findViewById(R.id.vncHelp);
        vncGroup = findViewById(R.id.vncGroup);
        AdapterView.OnItemSelectedListener upd = new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { updateRemote(); }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        };
        sound.setOnItemSelectedListener(upd);
        nic.setOnItemSelectedListener(upd);
        vnc.setOnCheckedChangeListener((b, c) -> updateRemote());
        vncLan.setOnCheckedChangeListener((b, c) -> updateRemote());
        vncPort.addTextChangedListener(new android.text.TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence t, int a, int b, int c) {}

            @Override
            public void onTextChanged(CharSequence t, int a, int b, int c) {}

            @Override
            public void afterTextChanged(android.text.Editable e) { if (fwd != null) updateRemote(); }
        });
        fwd = new ForwardList(this, fwdList, forwards, this::save);
        findViewById(R.id.fwdAdd).setOnClickListener(v -> fwd.addDialog());

        load();
        updateMode();

        List<String> abis = Arrays.asList(Build.SUPPORTED_ABIS);
        StringBuilder sb = new StringBuilder();
        sb.append("Aparelho: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(", Android ").append(Build.VERSION.RELEASE).append(", ABI ").append(abis.get(0));
        if (!abis.contains("arm64-v8a"))
            sb.append("\nAVISO: a biblioteca nativa deste APK só foi compilada para arm64-v8a.");
        sb.append("\nBIOS embutida: SeaBIOS (bios-256k.bin + vgabios-stdvga.bin).");
        sb.append("\nPasta do app para discos: ").append(diskDir()).append(" (Configurações › Discos decide se o app pergunta a pasta).");
        info.setText(sb);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!starting) resetStart();
    }

    @Override
    protected void onPause() {
        super.onPause();
        save();
    }

    private Architecture selectedArch() {
        return ARCHS[Math.max(0, arch.getSelectedItemPosition())];
    }

    private boolean isX86() {
        return AppFiles.isX86(selectedArch());
    }

    private boolean bios() {
        return isX86() && modeBios.isChecked();
    }

    /* ---- som, rede e VNC ---- */

    private void updateRemote() {
        mic.setVisibility(selectedSound() == VmSettings.SOUND_NONE ? View.GONE : View.VISIBLE);
        int n = selectedNic();
        netHelp.setText(n == VmSettings.NIC_NONE ? "A VM fica sem rede."
                : "Rede NAT: a VM recebe 10.0.2.15 por DHCP e acessa a internet pela conexão do aparelho. "
                + "O próprio aparelho aparece para ela como 10.0.2.2.");
        vncGroup.setVisibility(vnc.isChecked() ? View.VISIBLE : View.GONE);
        String ip = VmLauncher.lanAddress();
        int v;
        try {
            v = Integer.parseInt(vncPort.getText().toString().trim());
        } catch (NumberFormatException e) {
            v = -2;
        }
        int port = VmSettings.vncTcpPort(v);
        String host = vncLan.isChecked() ? (ip != null ? ip : "IP-do-aparelho") : "127.0.0.1";
        if (port < 0) {
            vncHelp.setText("Valor inválido. Use um número de tela de 0 a 99 (1 = porta 5901, como no cliente VNC) "
                    + "ou uma porta de 1024 a 65535: o Android não deixa apps usarem portas abaixo de 1024.");
            vncHelp.setTextColor(getColor(R.color.danger));
        } else {
            String addr = port - 5900 >= 0 && port - 5900 <= 99 ? host + ":" + (port - 5900) + "  (ou " + host + "::" + port + ")"
                    : host + "::" + port;
            vncHelp.setText(vncLan.isChecked()
                    ? "No cliente VNC (TigerVNC, RealVNC, bVNC…) conecte em " + addr
                    + ", com os dois aparelhos na mesma rede Wi-Fi. Use senha."
                    : "Só clientes VNC deste aparelho conectam: " + addr + ". Ligue a rede local para usar de outro dispositivo.");
            vncHelp.setTextColor(getColor(R.color.text2));
        }
        fwd.render(n == VmSettings.NIC_NONE);
    }

    private void updateMode() {
        boolean x86 = isX86();
        modeBios.setEnabled(x86);
        if (!x86 && modeBios.isChecked()) modeKernel.setChecked(true);
        boolean b = bios();
        biosGroup.setVisibility(b ? View.VISIBLE : View.GONE);
        kernelGroup.setVisibility(b ? View.GONE : View.VISIBLE);
        ctrlGroup.setVisibility(b ? View.VISIBLE : View.GONE);
        lists[VmSettings.KIND_FLOPPY].setVisibility(x86 ? View.VISIBLE : View.GONE);
        cmdline.setHint("console=" + selectedArch().linuxConsole());
        dtbGroup.setVisibility(x86 ? View.GONE : View.VISIBLE);
        archHelp.setText(archHelpText());
        if (choicesX86 == null || choicesX86 != x86)
            updateCardChoices(nicIdx == null ? settings.nic : selectedNic(), sndIdx == null ? settings.sound : selectedSound());
        renderQuickParams();
        int ataDevices = lists[0].items().size() + lists[1].items().size();
        if (sata.isChecked())
            ctrlHelp.setText("SATA/AHCI: Windows Vista ou mais novo e Linux. Até 8 discos e CDs.");
        else if (virtioCtrl.isChecked())
            ctrlHelp.setText("virtio-blk: só Linux (e BSDs), o disco mais rápido. Os CDs ficam no IDE. "
                    + "Windows precisa de driver próprio e não dá boot por ele.");
        else if (ataDevices > 4)
            ctrlHelp.setText("O IDE tem 4 vagas: as mídias excedentes vão para o SATA/AHCI (o Windows XP não as enxerga).");
        else
            ctrlHelp.setText("IDE: compatível com tudo (Windows XP, ReactOS, Linux). 4 vagas.");
        updateChips();
    }

    private void updateChips() {
        if (chipArch == null || lists == null) return;
        chipArch.setText(AppFiles.ARCH_NAMES[Math.max(0, arch.getSelectedItemPosition())]);
        chipRam.setText(ram.getText().toString().trim() + " MiB");
        int mb;
        try {
            mb = Integer.parseInt(ram.getText().toString().trim());
        } catch (NumberFormatException e) {
            mb = -1;
        }
        String low = AppFiles.lowRamWarning(mb);
        ramHelp.setText(low);
        ramHelp.setVisibility(low != null ? View.VISIBLE : View.GONE);
        chipMode.setText(bios() ? "BIOS" : "Kernel Linux");
        int d = lists[0].items().size(), c = lists[1].items().size(), f = isX86() ? lists[2].items().size() : 0;
        StringBuilder sb = new StringBuilder();
        if (d > 0) sb.append(d).append(d == 1 ? " disco" : " discos");
        if (c > 0) sb.append(sb.length() > 0 ? " · " : "").append(c).append(c == 1 ? " ISO" : " ISOs");
        if (f > 0) sb.append(sb.length() > 0 ? " · " : "").append(f).append(f == 1 ? " disquete" : " disquetes");
        chipMedia.setText(sb.length() > 0 ? sb : "sem mídias");
    }

    /* ---- preferencias ---- */

    private void load() {
        loading = true;
        title.setText(settings.name);
        arch.setSelection(Math.max(0, Math.min(ARCHS.length - 1, settings.arch)));
        ram.setText(settings.ram);
        if (settings.bios) modeBios.setChecked(true);
        else modeKernel.setChecked(true);
        cmdline.setText(settings.cmdline);
        fbEnable.setChecked(settings.fb);
        fbSize.setText(settings.fbSize);
        int ri = Arrays.asList(AppSettings.RESOLUTIONS).indexOf(settings.fbSize.trim());
        fbRes.setSelection(ri >= 0 ? ri : AppSettings.RESOLUTIONS.length);
        updateFb();
        rtcMode.setSelection(settings.rtc);
        rtcDate.setText(settings.rtcDate);
        rtcDate.setVisibility(settings.rtc == VmSettings.RTC_FIXED ? View.VISIBLE : View.GONE);
        rawAddr.setText(settings.rawAddr);
        setDtb(settings.dtb);
        if (settings.sata) sata.setChecked(true);
        else if (settings.virtio) virtioCtrl.setChecked(true);
        else ((RadioButton) findViewById(R.id.ide)).setChecked(true);
        setKernel(settings.kernel);
        setInitrd(settings.initrd);
        for (int k = 0; k < lists.length; k++) lists[k].setItems(settings.media[k]);

        /* ordem de boot: opcoes fixas; um valor antigo digitado a mao vira "Personalizada" */
        bootCodes.clear();
        bootCodes.addAll(Arrays.asList(VmSettings.BOOT_CODES));
        List<String> names = new ArrayList<>(Arrays.asList(VmSettings.BOOT_NAMES));
        int idx = VmSettings.bootIndex(settings.bootOrder);
        if (idx < 0) {
            bootCodes.add(settings.bootOrder);
            names.add("Personalizada (" + settings.bootOrder + ")");
            idx = bootCodes.size() - 1;
        }
        bootOrder.setAdapter(spinnerAdapter(names.toArray(new String[0])));
        bootOrder.setSelection(idx);

        choicesX86 = null;
        updateCardChoices(settings.nic, settings.sound);
        mic.setChecked(settings.mic);
        forwards.clear();
        forwards.addAll(settings.forwards);
        vnc.setChecked(settings.vnc);
        vncLan.setChecked(settings.vncLan);
        vncPort.setText(String.valueOf(settings.vncPort));
        vncPassword.setText(settings.vncPassword);
        updateRemote();
        loading = false;
    }

    private void save() {
        if (settings == null || loading) return;
        String n = title.getText().toString().trim();
        if (!n.isEmpty()) settings.name = n;
        settings.arch = arch.getSelectedItemPosition();
        settings.ram = ram.getText().toString();
        settings.bios = modeBios.isChecked();
        settings.cmdline = cmdline.getText().toString();
        settings.fb = fbEnable.isChecked();
        settings.fbSize = fbSize.getText().toString();
        int b = bootOrder.getSelectedItemPosition();
        settings.bootOrder = b >= 0 && b < bootCodes.size() ? bootCodes.get(b) : "";
        settings.sata = sata.isChecked();
        settings.virtio = virtioCtrl.isChecked();
        settings.kernel = kernel;
        settings.initrd = initrd;
        for (int k = 0; k < lists.length; k++) {
            settings.media[k].clear();
            settings.media[k].addAll(lists[k].items());
        }
        settings.sound = selectedSound();
        settings.nic = selectedNic();
        settings.rtc = Math.max(0, rtcMode.getSelectedItemPosition());
        settings.rtcDate = rtcDate.getText().toString().trim();
        settings.rawAddr = rawAddr.getText().toString().trim();
        settings.dtb = dtb;
        settings.mic = mic.isChecked();
        settings.forwards.clear();
        settings.forwards.addAll(forwards);
        settings.vnc = vnc.isChecked();
        settings.vncLan = vncLan.isChecked();
        try {
            int p = Integer.parseInt(vncPort.getText().toString().trim());
            if (p >= 0 && p < 65536) settings.vncPort = p; /* tela (0-99) ou porta; validado ao iniciar */
        } catch (NumberFormatException ignored) {
        }
        settings.vncPassword = vncPassword.getText().toString();
        settings.save(this);
    }

    /* ---- arquivos ---- */

    private void setKernel(String v) {
        kernel = v;
        kernelName.setText(v == null ? "(nenhum)" : displayName(v));
    }

    private void setInitrd(String v) {
        initrd = v;
        initrdName.setText(v == null ? "(nenhum)" : displayName(v));
    }

    private void setDtb(String v) {
        dtb = v;
        dtbName.setText(v == null ? "(gerado automaticamente)" : displayName(v));
    }

    /* ---- tela grafica, placas e parametros do kernel ---- */

    private void updateFb() {
        boolean on = fbEnable.isChecked();
        int pos = fbRes.getSelectedItemPosition();
        boolean custom = pos < 0 || pos >= AppSettings.RESOLUTIONS.length;
        fbRes.setVisibility(on ? View.VISIBLE : View.GONE);
        fbSize.setVisibility(on && custom ? View.VISIBLE : View.GONE);
        if (!custom && !loading) fbSize.setText(AppSettings.RESOLUTIONS[pos]);
    }

    private String archHelpText() {
        switch (selectedArch()) {
            case ARM64:
                return "Máquina virt: CPU Cortex-A53 (ARMv8, 64 bits), GICv2, timer genérico, serial PL011 (ttyAMA0), "
                        + "RTC PL031 e dispositivos virtio (discos /dev/vda…, rede e som). Inicia um kernel Linux "
                        + "(Image ou Image.gz) com initrd e device tree opcionais.";
            case ARM:
                return "Máquina virt: CPU Cortex-A15 (ARMv7, 32 bits), GICv2, timer genérico, serial PL011 (ttyAMA0), "
                        + "RTC PL031 e dispositivos virtio (discos /dev/vda…, rede e som). Inicia um kernel Linux "
                        + "(zImage) com initrd e device tree opcionais.";
            default:
                return "PC: BIOS SeaBIOS, vídeo VGA/VBE, IDE/SATA, ACPI, APIC. Também inicia um kernel Linux direto.";
        }
    }

    /** Monta as listas de placas da arquitetura, mantendo a escolha quando ela existe nas duas. */
    private void updateCardChoices(int nicSel, int sndSel) {
        boolean x86 = isX86();
        choicesX86 = x86;
        nicIdx = VmSettings.nicChoices(x86);
        sndIdx = VmSettings.soundChoices(x86);
        nic.setAdapter(spinnerAdapter(choiceNames(VmSettings.NIC_NAMES, nicIdx, x86)));
        sound.setAdapter(spinnerAdapter(choiceNames(VmSettings.SOUND_NAMES, sndIdx, x86)));
        nic.setSelection(Math.max(0, indexOf(nicIdx, nicSel)));
        sound.setSelection(Math.max(0, indexOf(sndIdx, sndSel)));
    }

    private static String[] choiceNames(String[] all, int[] idx, boolean x86) {
        String[] out = new String[idx.length];
        for (int i = 0; i < idx.length; i++) out[i] = idx[i] == 0 && !x86 ? "Automática (virtio)" : all[idx[i]];
        return out;
    }

    private static int indexOf(int[] a, int v) {
        for (int i = 0; i < a.length; i++) if (a[i] == v) return i;
        return -1;
    }

    private int selectedNic() {
        int p = nic.getSelectedItemPosition();
        return nicIdx != null && p >= 0 && p < nicIdx.length ? nicIdx[p] : 0;
    }

    private int selectedSound() {
        int p = sound.getSelectedItemPosition();
        return sndIdx != null && p >= 0 && p < sndIdx.length ? sndIdx[p] : 0;
    }

    private List<String> cmdTokens() {
        List<String> t = new ArrayList<>();
        for (String x : cmdline.getText().toString().trim().split("\\s+")) if (!x.isEmpty()) t.add(x);
        return t;
    }

    /** Liga/desliga um parametro; root= e init= sao exclusivos. Linha vazia ganha o console padrao. */
    private void toggleParam(String p) {
        List<String> t = cmdTokens();
        if (t.isEmpty()) t.add("console=" + selectedArch().linuxConsole());
        if (t.contains(p)) {
            t.remove(p);
        } else {
            String key = p.contains("=") ? p.substring(0, p.indexOf('=') + 1) : null;
            if ("root=".equals(key) || "init=".equals(key)) t.removeIf(x -> x.startsWith(key));
            t.add(p);
        }
        cmdline.setText(String.join(" ", t));
        cmdline.setSelection(cmdline.length());
    }

    private void renderQuickParams() {
        if (quickParams == null) return;
        quickParams.removeAllViews();
        List<String> t = cmdTokens();
        for (String[] q : QUICK) {
            boolean on = t.contains(q[0]);
            TextView chip = new TextView(this, null, 0, R.style.Chip);
            chip.setText(on ? "✓ " + q[1] : q[1]);
            chip.setBackgroundResource(R.drawable.chip_bg);
            if (on) {
                chip.setBackgroundTintList(android.content.res.ColorStateList.valueOf(getColor(R.color.accent)));
                chip.setTextColor(getColor(R.color.on_accent));
            }
            chip.setOnClickListener(v -> toggleParam(q[0]));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.setMarginEnd((int) (6 * getResources().getDisplayMetrics().density));
            quickParams.addView(chip, lp);
        }
    }

    private static AdapterView.OnItemSelectedListener onSelect(Runnable r) {
        return new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { r.run(); }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        };
    }

    private void pick(int req, boolean writable) {
        Intent it = AppFiles.pickIntent(writable);
        if (req == REQ_CONVERT) it.setFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(it, req);
        } catch (Exception ex) {
            toast("Não há seletor de arquivos: " + ex.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (diskCreator.onActivityResult(requestCode, resultCode, data)) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == REQ_CONVERT) {
            convertDialog(data.getData());
            return;
        }
        String uri = AppFiles.takePersistable(this, data);
        if (requestCode == REQ_KERNEL) setKernel(uri);
        else if (requestCode == REQ_INITRD) setInitrd(uri);
        else if (requestCode == REQ_DTB) setDtb(uri);
        else if (requestCode >= REQ_MEDIA && requestCode < REQ_MEDIA + lists.length) lists[requestCode - REQ_MEDIA].add(uri);
        save();
    }

    private String displayName(String f) {
        return AppFiles.displayName(this, f);
    }

    private static String sizeStr(long b) {
        return AppFiles.sizeStr(b);
    }

    private File diskDir() {
        return AppFiles.diskDir(this);
    }

    /* ---- MediaListView.Host ---- */

    @Override
    public void pickMedia(MediaListView list) {
        pick(REQ_MEDIA + list.kind(), list.kind() != VmSettings.KIND_CD);
    }

    @Override
    public void newDisk(MediaListView list) {
        diskCreator.ask("Novo disco (MVD)", "disco.mvd", 8, null, uri -> {
            list.add(uri);
            toast("Criado: " + (uri.startsWith("/") ? uri : displayName(uri)));
        });
    }

    @Override
    public void convertDisk(MediaListView list) {
        pick(REQ_CONVERT, false);
    }

    @Override
    public void editPending(MediaListView list, int index) {}

    @Override
    public void onMediaChanged(MediaListView list) {
        updateMode();
        save();
    }

    /* ---- conversao de imagens ---- */

    /** Pergunta o nome, a compressao e o local; converte para MVD. */
    private void convertDialog(Uri src) {
        String shown = displayName(src.toString());
        String base = shown.replaceAll("  \\(.*\\)$", "");
        int dot = base.lastIndexOf('.');
        if (dot > 0) base = base.substring(0, dot);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        TextView from = new TextView(this);
        from.setText("Origem: " + shown);
        EditText name = new EditText(this);
        name.setHint("nome do disco novo");
        name.setText(base.replace('/', '_') + ".mvd");
        CheckBox compress = new CheckBox(this);
        compress.setText("Comprimir (LZ4): ocupa menos; blocos regravados voltam a ocupar o normal");
        android.widget.RadioGroup where = diskCreator.locationChooser();
        box.addView(from);
        box.addView(name);
        box.addView(compress);
        box.addView(where);
        android.widget.ScrollView sv = new android.widget.ScrollView(this);
        sv.addView(box);
        new AlertDialog.Builder(this)
                .setTitle("Converter para MVD")
                .setView(sv)
                .setPositiveButton("Converter", (d, w) -> {
                    String n = name.getText().toString().trim().replace('/', '_');
                    if (n.isEmpty()) {
                        toast("nome inválido");
                        return;
                    }
                    File dir = diskDir();
                    if (!dir.isDirectory() && !dir.mkdirs()) {
                        toast("não consegui criar " + dir);
                        return;
                    }
                    if (DiskCreator.choosesFolder(where)) {
                        /* converte num temporario da pasta do app e copia para a pasta escolhida */
                        File tmp = new File(dir, ".convertendo-" + System.nanoTime() + ".mvd");
                        diskCreator.saveAs(n, doc -> runConversion(src, tmp, compress.isChecked(), doc));
                        return;
                    }
                    File dst = new File(dir, n);
                    if (dst.exists()) {
                        toast(n + " já existe");
                        return;
                    }
                    runConversion(src, dst, compress.isChecked(), null);
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    /** doc != null: dst e temporario e o resultado vai para esse documento (pasta escolhida). */
    private void runConversion(Uri src, File dst, boolean compress, Uri doc) {
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        TextView status = new TextView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(bar);
        box.addView(status);
        final boolean[] cancel = {false};
        String shownName = doc != null ? displayName(doc.toString()) : dst.getName();
        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("Convertendo " + shownName)
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancelar", (d, w) -> cancel[0] = true)
                .show();
        long t0 = System.nanoTime();
        new Thread(() -> {
            String error = null;
            try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(src, "r")) {
                if (pfd == null) throw new IOException("não foi possível abrir a origem");
                final long[] last = {0};
                DiskImages.convert(pfd, dst.getAbsolutePath(), DiskImages.Format.MVD, compress, (done, total) -> {
                    long now = System.nanoTime();
                    if (now - last[0] > 200_000_000L || done == total) {
                        last[0] = now;
                        double secs = (now - t0) / 1e9;
                        runOnUiThread(() -> {
                            bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 1000);
                            status.setText(sizeStr(done) + " de " + sizeStr(total)
                                    + String.format(java.util.Locale.ROOT, "  (%.0f s)", secs));
                        });
                    }
                    return !cancel[0];
                });
                if (doc != null) {
                    if (cancel[0]) throw new IOException("cancelado");
                    runOnUiThread(() -> status.setText("Copiando para a pasta escolhida…"));
                    final long[] last2 = {0};
                    diskCreator.copyToDocument(dst, doc, (done, total) -> {
                        long now = System.nanoTime();
                        if (now - last2[0] > 200_000_000L || done == total) {
                            last2[0] = now;
                            runOnUiThread(() -> {
                                bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 1000);
                                status.setText("Copiando: " + sizeStr(done) + " de " + sizeStr(total));
                            });
                        }
                    });
                }
            } catch (Exception ex) {
                error = ex.getMessage() != null ? ex.getMessage() : ex.toString();
                if (doc != null) diskCreator.deleteDocument(doc);
            } finally {
                if (doc != null) dst.delete();
            }
            final String err = error;
            runOnUiThread(() -> {
                dlg.dismiss();
                if (err != null) {
                    error("Falha na conversão", new IOException(err));
                    return;
                }
                String result = doc != null ? doc.toString() : dst.getAbsolutePath();
                lists[VmSettings.KIND_DISK].add(result);
                String msg = "Convertido: " + (doc != null ? displayName(result) : result);
                if (doc == null) {
                    try {
                        DiskImages.Info i = DiskImages.info(dst.getAbsolutePath());
                        msg += "\nDisco de " + sizeStr(i.virtualSize) + ", arquivo de " + sizeStr(i.fileSize);
                    } catch (IOException ignored) {
                    }
                }
                new AlertDialog.Builder(this).setTitle("Conversão concluída").setMessage(msg)
                        .setPositiveButton("OK", null).show();
            });
        }, "converter").start();
    }

    /* ---- inicio da VM ---- */

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != REQ_MIC)
            return;
        if (results.length == 0 || results[0] != android.content.pm.PackageManager.PERMISSION_GRANTED)
            toast("Sem permissão do microfone: a VM vai ouvir silêncio.");
        micAsked = true; /* sem a permissao o VmLauncher deixa o microfone desligado */
        onStartClicked();
    }

    private void onStartClicked() {
        if (VmHolder.isRunning(settings.id)) {
            startActivity(new Intent(this, VmActivity.class));
            return;
        }
        if (starting || VmHolder.vm != null) return;
        save();
        if (!micAsked && VmLauncher.needsMicPermission(this, settings)) {
            /* pede o microfone; inicia na resposta (negado = o convidado ouve silencio) */
            requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO}, REQ_MIC);
            return;
        }
        starting = true;
        start.setEnabled(false);
        start.setText("Preparando...");
        VmLauncher.start(this, settings, err -> {
            resetStart();
            if (err != null) error("Não foi possível iniciar a VM", err);
        });
    }

    private void resetStart() {
        if (start == null) return;
        starting = false;
        boolean mine = VmHolder.isRunning(settings.id);
        boolean other = VmHolder.vm != null && !mine;
        start.setEnabled(!other);
        start.setText(mine ? "Voltar para a VM em execução"
                : other ? "Outra VM em execução (" + VmHolder.vmName + ")" : "Iniciar VM");
        start.setCompoundDrawablesRelativeWithIntrinsicBounds(mine ? R.drawable.ic_monitor : R.drawable.ic_play, 0, 0, 0);
    }

    private ArrayAdapter<String> spinnerAdapter(String[] items) {
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return ad;
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    private void error(String title, Throwable ex) {
        String msg = ex.getMessage() != null ? ex.getMessage() : ex.toString();
        new AlertDialog.Builder(this).setTitle(title).setMessage(msg).setPositiveButton("OK", null).show();
    }
}
