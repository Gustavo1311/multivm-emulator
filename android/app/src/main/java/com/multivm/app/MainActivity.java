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
public class MainActivity extends Activity implements MediaListView.Host {

    private static final Architecture[] ARCHS = AppFiles.ARCHS;
    /** extra do Intent: id da VM a editar (VmSettings) */
    static final String EXTRA_VM_ID = "vmId";

    /* requestCodes do seletor de arquivos */
    private static final int REQ_KERNEL = 0, REQ_INITRD = 1, REQ_MEDIA = 10, REQ_CONVERT = 100;

    private VmSettings settings;
    private Spinner arch, bootOrder;
    private EditText ram, cmdline, fbSize;
    private SeekBar ramBar;
    private RadioButton modeBios, modeKernel, sata;
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
        cmdline = findViewById(R.id.cmdline);
        fbSize = findViewById(R.id.fbSize);
        modeBios = findViewById(R.id.modeBios);
        modeKernel = findViewById(R.id.modeKernel);
        sata = findViewById(R.id.sata);
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
        AppFiles.bindRam(ram, ramBar, this::updateChips);
        fbEnable.setOnCheckedChangeListener((b, c) -> fbSize.setVisibility(c ? View.VISIBLE : View.GONE));

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
        sound.setAdapter(spinnerAdapter(VmSettings.SOUND_NAMES));
        nic.setAdapter(spinnerAdapter(VmSettings.NIC_NAMES));
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
        sb.append("\nNovos discos ficam em ").append(diskDir());
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
        mic.setVisibility(sound.getSelectedItemPosition() == VmSettings.SOUND_NONE ? View.GONE : View.VISIBLE);
        int n = nic.getSelectedItemPosition();
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
        int ataDevices = lists[0].items().size() + lists[1].items().size();
        if (sata.isChecked())
            ctrlHelp.setText("SATA/AHCI: Windows Vista ou mais novo e Linux. Até 8 discos e CDs.");
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
        fbSize.setVisibility(settings.fb ? View.VISIBLE : View.GONE);
        if (settings.sata) sata.setChecked(true);
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

        sound.setSelection(settings.sound);
        nic.setSelection(settings.nic);
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
        settings.kernel = kernel;
        settings.initrd = initrd;
        for (int k = 0; k < lists.length; k++) {
            settings.media[k].clear();
            settings.media[k].addAll(lists[k].items());
        }
        settings.sound = Math.max(0, sound.getSelectedItemPosition());
        settings.nic = Math.max(0, nic.getSelectedItemPosition());
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
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == REQ_CONVERT) {
            convertDialog(data.getData());
            return;
        }
        String uri = AppFiles.takePersistable(this, data);
        if (requestCode == REQ_KERNEL) setKernel(uri);
        else if (requestCode == REQ_INITRD) setInitrd(uri);
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
        AppFiles.diskDialog(this, "Novo disco (MVD)", "disco.mvd", 8, (name, gb) -> {
            try {
                String path = AppFiles.createMvd(this, name, gb);
                list.add(path);
                toast("Criado: " + path);
            } catch (Exception ex) {
                error("Falha ao criar o disco", ex);
            }
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

    /** Pergunta o nome e a compressao; converte para MVD na pasta de discos. */
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
        box.addView(from);
        box.addView(name);
        box.addView(compress);
        new AlertDialog.Builder(this)
                .setTitle("Converter para MVD")
                .setView(box)
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
                    File dst = new File(dir, n);
                    if (dst.exists()) {
                        toast(n + " já existe");
                        return;
                    }
                    runConversion(src, dst, compress.isChecked());
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void runConversion(Uri src, File dst, boolean compress) {
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
        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("Convertendo " + dst.getName())
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
            } catch (Exception ex) {
                error = ex.getMessage();
            }
            final String err = error;
            runOnUiThread(() -> {
                dlg.dismiss();
                if (err != null) {
                    error("Falha na conversão", new IOException(err));
                    return;
                }
                lists[VmSettings.KIND_DISK].add(dst.getAbsolutePath());
                String msg = "Convertido: " + dst.getAbsolutePath();
                try {
                    DiskImages.Info i = DiskImages.info(dst.getAbsolutePath());
                    msg += "\nDisco de " + sizeStr(i.virtualSize) + ", arquivo de " + sizeStr(i.fileSize);
                } catch (IOException ignored) {
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
