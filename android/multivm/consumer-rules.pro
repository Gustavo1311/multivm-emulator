# Metodos chamados a partir do codigo nativo
-keep class com.multivm.core.NativeBridge { *; }
-keep interface com.multivm.core.NativeBridge$* { *; }
-keepclassmembers class * implements com.multivm.core.NativeBridge$Callbacks { void onSerial(byte[]); }
-keepclassmembers class * implements com.multivm.core.NativeBridge$LogListener { void onLog(int, java.lang.String); }
-keep class com.multivm.core.DiskImages$Info { <init>(java.lang.String, long, long, boolean); }
-keepclassmembers class * implements com.multivm.core.DiskImages$Progress { boolean onProgress(long, long); }
