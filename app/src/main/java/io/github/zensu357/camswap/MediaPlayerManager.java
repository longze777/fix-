package io.github.zensu357.camswap;

import android.media.MediaPlayer;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.view.Surface;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import io.github.zensu357.camswap.utils.LogUtil;
import io.github.zensu357.camswap.utils.VideoManager;

/**
 * Manages all player backends, GLVideoRenderer, and SurfaceRelay instances.
 * Centralizes player lifecycle, restart, rotation, and release logic.
 * <p>
 * In local mode each slot gets its own {@link AndroidMediaPlayerBackend}.
 * In stream mode all slots share frames from a single {@link ExoPlayerBackend}
 * routed through their respective GL renderers.
 */
public final class MediaPlayerManager {
    private final Object mediaLock = new Object();
    private final Map<MediaPlayer, ParcelFileDescriptor> activePfds = new ConcurrentHashMap<>();
    private String currentPackageName;
    private volatile long lastCamera2PlaybackStartRealtimeMs;
    private volatile int lastCamera2PlaybackDurationMs;

    // ---- Camera1 players (created by Camera1Handler) ----
    MediaPlayer mplayer1;
    MediaPlayer mMediaPlayer;
    GLVideoRenderer c1_renderer_holder;
    GLVideoRenderer c1_renderer_texture;

    // ---- Camera2 preview players ----
    MediaPlayer c2_player;
    MediaPlayer c2_player_1;
    GLVideoRenderer c2_renderer;
    GLVideoRenderer c2_renderer_1;
    SurfaceRelay c2_relay;
    SurfaceRelay c2_relay_1;

    // ---- Camera2 reader players ----
    MediaPlayer c2_reader_player;
    MediaPlayer c2_reader_player_1;
    GLVideoRenderer c2_reader_renderer;
    GLVideoRenderer c2_reader_renderer_1;
    SurfaceRelay c2_reader_relay;
    SurfaceRelay c2_reader_relay_1;

    // Per-slot surface tracking: skip re-init when surface unchanged
    private Surface lastC2ReaderSurface, lastC2ReaderSurface1;
    private Surface lastC2PreviewSurface, lastC2PreviewSurface1;

    // ---- Stream mode: single shared ExoPlayerBackend ----
    private SurfacePlayerBackend streamBackend;

    /** Set current package name (future per-app video). */
    public void setPackageName(String packageName) {
        this.currentPackageName = packageName;
    }

    /**
     * Central video path query.
     */
    String getVideoPath() {
        return VideoManager.getCurrentVideoPath();
    }

    /** Get current media source descriptor from config. */
    MediaSourceDescriptor getMediaSource() {
        return VideoManager.getCurrentMediaSource();
    }

    /** Whether we are currently in stream mode. */
    boolean isStreamMode() {
        return VideoManager.isStreamMode();
    }

    // =====================================================================
    // Camera2 player initialization
    // =====================================================================

    /**
     * Initialize Camera2 players for the given surfaces.
     * In stream mode, creates a single ExoPlayerBackend for the primary preview
     * and routes frames to reader surfaces via GL renderers.
     */
    void initCamera2Players(Surface readerSurface, Surface readerSurface1,
            Surface previewSurface, Surface previewSurface1) {

        MediaSourceDescriptor source = getMediaSource();

        if (source.isStream()) {
            initCamera2PlayersStream(readerSurface, readerSurface1,
                    previewSurface, previewSurface1, source);
        } else {
            initCamera2PlayersLocal(readerSurface, readerSurface1,
                    previewSurface, previewSurface1);
        }
    }

    private void initCamera2PlayersLocal(Surface readerSurface, Surface readerSurface1,
            Surface previewSurface, Surface previewSurface1) {
        if (readerSurface != null && readerSurface != lastC2ReaderSurface) {
            c2_reader_player = recreatePlayer(c2_reader_player);
            GLVideoRenderer[] r = { c2_reader_renderer };
            SurfaceRelay[] rr = { c2_reader_relay };
            setupMediaPlayer(c2_reader_player, r, rr, readerSurface, "c2_reader", false);
            c2_reader_renderer = r[0];
            c2_reader_relay = rr[0];
            lastC2ReaderSurface = readerSurface;
        }
        if (readerSurface1 != null && readerSurface1 != lastC2ReaderSurface1) {
            c2_reader_player_1 = recreatePlayer(c2_reader_player_1);
            GLVideoRenderer[] r = { c2_reader_renderer_1 };
            SurfaceRelay[] rr = { c2_reader_relay_1 };
            setupMediaPlayer(c2_reader_player_1, r, rr, readerSurface1, "c2_reader_1", false);
            c2_reader_renderer_1 = r[0];
            c2_reader_relay_1 = rr[0];
            lastC2ReaderSurface1 = readerSurface1;
        }

        boolean playSound = VideoManager.getConfig().getBoolean(ConfigManager.KEY_PLAY_VIDEO_SOUND, false);

        if (previewSurface != null && previewSurface != lastC2PreviewSurface) {
            c2_player = recreatePlayer(c2_player);
            GLVideoRenderer[] r = { c2_renderer };
            SurfaceRelay[] rr = { c2_relay };
            setupMediaPlayer(c2_player, r, rr, previewSurface, "c2_preview", playSound);
            c2_renderer = r[0];
            c2_relay = rr[0];
            lastC2PreviewSurface = previewSurface;
        }
        if (previewSurface1 != null && previewSurface1 != lastC2PreviewSurface1) {
            c2_player_1 = recreatePlayer(c2_player_1);
            GLVideoRenderer[] r = { c2_renderer_1 };
            SurfaceRelay[] rr = { c2_relay_1 };
            setupMediaPlayer(c2_player_1, r, rr, previewSurface1, "c2_preview_1", playSound);
            c2_renderer_1 = r[0];
            c2_relay_1 = rr[0];
            lastC2PreviewSurface1 = previewSurface1;
        }
        LogUtil.log("【CS】Camera2处理过程完全执行（本地模式）");
    }

    private void initCamera2PlayersStream(Surface readerSurface, Surface readerSurface1,
            Surface previewSurface, Surface previewSurface1,
            MediaSourceDescriptor source) {
        // Release any old stream backend
        releaseStreamBackend();

        // 所有渲染器旋转为 0°，rotation_offset 仅通过 captureFrameForYuv 应用于 YUV 截帧

        // Choose primary surface: prefer preview, fallback to reader
        Surface primaryTarget = previewSurface != null ? previewSurface : readerSurface;
        if (primaryTarget == null) {
            LogUtil.log("【CS】流模式：无可用目标 Surface");
            return;
        }

        // Set up GL renderers for all surfaces (stream frames are shared)
        if (readerSurface != null) {
            GLVideoRenderer.releaseSafely(c2_reader_renderer);
            SurfaceRelay.releaseSafely(c2_reader_relay);
            c2_reader_renderer = GLVideoRenderer.createSafely(readerSurface, "c2_reader_stream");
        }
        if (readerSurface1 != null) {
            GLVideoRenderer.releaseSafely(c2_reader_renderer_1);
            SurfaceRelay.releaseSafely(c2_reader_relay_1);
            c2_reader_renderer_1 = GLVideoRenderer.createSafely(readerSurface1, "c2_reader_1_stream");
        }
        if (previewSurface != null) {
            GLVideoRenderer.releaseSafely(c2_renderer);
            SurfaceRelay.releaseSafely(c2_relay);
            c2_renderer = GLVideoRenderer.createSafely(previewSurface, "c2_preview_stream");
        }
        if (previewSurface1 != null) {
            GLVideoRenderer.releaseSafely(c2_renderer_1);
            SurfaceRelay.releaseSafely(c2_relay_1);
            c2_renderer_1 = GLVideoRenderer.createSafely(previewSurface1, "c2_preview_1_stream");
        }

        // Create stream backend — output to primary GL renderer's input surface
        try {
            streamBackend = createStreamBackend();
            GLVideoRenderer primaryRenderer = (previewSurface != null) ? c2_renderer : c2_reader_renderer;
            Surface backendSurface;
            if (primaryRenderer != null && primaryRenderer.isInitialized()) {
                backendSurface = primaryRenderer.getInputSurface();
            } else {
                backendSurface = primaryTarget;
            }
            streamBackend.setOutputSurface(backendSurface);
            streamBackend.setVolume(VideoManager.getConfig().getBoolean(
                    ConfigManager.KEY_PLAY_VIDEO_SOUND, false) ? 1.0f : 0f);
            streamBackend.setLooping(false); // streams don't loop
            streamBackend.setListener(new SurfacePlayerBackend.Listener() {
                @Override
                public void onReady() {
                    LogUtil.log("【CS】流播放器就绪");
                    lastCamera2PlaybackStartRealtimeMs = SystemClock.elapsedRealtime();
                }

                @Override
                public void onError(String message, Throwable cause) {
                    LogUtil.log("【CS】流播放器错误: " + message
                            + (cause != null ? " " + cause : ""));
                }

                @Override
                public void onDisconnected() {
                    LogUtil.log("【CS】流断开连接");
                }

                @Override
                public void onReconnected() {
                    LogUtil.log("【CS】流重连成功");
                }

                @Override
                public void onCompletion() {
                    LogUtil.log("【CS】流播放完成");
                }
            });
            streamBackend.open(source);
            LogUtil.log("【CS】Camera2处理过程完全执行（流模式: " + source.streamUrl + "）");
        } catch (Exception e) {
            LogUtil.log("【CS】流模式初始化失败: " + android.util.Log.getStackTraceString(e));
        }
    }

    private SurfacePlayerBackend createStreamBackend() {
        // Try to create ExoPlayerBackend via reflection to avoid hard compile dependency
        // when Media3 is not on classpath (should not happen with proper gradle setup)
        try {
            Class<?> clazz = Class.forName("io.github.zensu357.camswap.ExoPlayerBackend");
            return (SurfacePlayerBackend) clazz.getDeclaredConstructor().newInstance();
        } catch (Exception e) {
            LogUtil.log("【CS】ExoPlayerBackend 不可用，回退到 AndroidMediaPlayerBackend: " + e);
            return new AndroidMediaPlayerBackend();
        }
    }

    private void releaseStreamBackend() {
        if (streamBackend != null) {
            streamBackend.release();
            streamBackend = null;
        }
    }

    public void bindCamera1Pfd(MediaPlayer player, ParcelFileDescriptor pfd) {
        bindPfd(player, pfd);
    }

    public void closeCamera1Pfd(MediaPlayer player) {
        closePfd(player);
    }

    private void bindPfd(MediaPlayer player, ParcelFileDescriptor pfd) {
        if (player == null || pfd == null) return;
        ParcelFileDescriptor old = activePfds.put(player, pfd);
        if (old != null && old != pfd) {
            try {
                old.close();
            } catch (Exception ignored) {
            }
        }
    }

    private void closePfd(MediaPlayer player) {
        if (player == null) return;
        ParcelFileDescriptor pfd = activePfds.remove(player);
        if (pfd != null) {
            try {
                pfd.close();
            } catch (Exception ignored) {
            }
        }
    }

    private MediaPlayer recreatePlayer(MediaPlayer old) {
        if (old != null) {
            closePfd(old);
            old.release();
        }
        return new MediaPlayer();
    }

    long getCamera2PlaybackPositionMs() {
        // Stream mode: query stream backend
        if (streamBackend != null) {
            try {
                long pos = streamBackend.getCurrentPositionMs();
                if (pos > 0) return pos;
            } catch (Exception ignored) {
            }
        }

        MediaPlayer[] players = {
                c2_player, c2_player_1,
                c2_reader_player, c2_reader_player_1,
                mplayer1, mMediaPlayer
        };
        for (MediaPlayer player : players) {
            if (player == null) {
                continue;
            }
            try {
                int position = player.getCurrentPosition();
                if (position > 0) {
                    return position;
                }
            } catch (Exception ignored) {
            }
        }
        if (lastCamera2PlaybackStartRealtimeMs > 0) {
            long elapsedMs = Math.max(0L, SystemClock.elapsedRealtime() - lastCamera2PlaybackStartRealtimeMs);
            if (lastCamera2PlaybackDurationMs > 0) {
                return elapsedMs % lastCamera2PlaybackDurationMs;
            }
            return elapsedMs;
        }
        return 0;
    }

    private void markCamera2PlaybackStarted(MediaPlayer player, String tag) {
        if (tag == null || !tag.startsWith("c2_") || player == null) {
            return;
        }
        lastCamera2PlaybackStartRealtimeMs = SystemClock.elapsedRealtime();
        try {
            lastCamera2PlaybackDurationMs = Math.max(0, player.getDuration());
        } catch (Exception ignored) {
            lastCamera2PlaybackDurationMs = 0;
        }
    }

    // =====================================================================
    // Restart / rotation / release
    // =====================================================================

    /** Restart all active players with current video/stream. */
    void restartAll() {
        synchronized (mediaLock) {
            if (isStreamMode()) {
                // Stream mode: restart the single stream backend
                if (streamBackend != null) {
                    streamBackend.restart();
                }
            } else {
                // Local mode: restart individual MediaPlayers
                VideoManager.checkProviderAvailability();
                restartSinglePlayer(mplayer1, c1_renderer_holder, "mplayer1");
                restartSinglePlayer(mMediaPlayer, c1_renderer_texture, "mMediaPlayer");
                restartSinglePlayer(c2_reader_player, c2_reader_renderer, "c2_reader_player");
                restartSinglePlayer(c2_reader_player_1, c2_reader_renderer_1, "c2_reader_player_1");
                restartSinglePlayer(c2_player, c2_renderer, "c2_player");
                restartSinglePlayer(c2_player_1, c2_renderer_1, "c2_player_1");
            }
        }
    }

    private void restartSinglePlayer(MediaPlayer player, GLVideoRenderer renderer, String tag) {
        if (player == null)
            return;
        try {
            if (player.isPlaying())
                player.stop();
            player.reset();
            if (renderer != null && renderer.isInitialized()) {
                player.setSurface(renderer.getInputSurface());
            }
            android.os.ParcelFileDescriptor pfd = VideoManager.getVideoPFD();
            if (pfd != null) {
                bindPfd(player, pfd);
                player.setDataSource(pfd.getFileDescriptor());
            } else {
                closePfd(player);
                player.setDataSource(getVideoPath());
            }
            player.prepare();
            player.start();
        } catch (Exception e) {
            LogUtil.log("【CS】重启 " + tag + " 失败: " + android.util.Log.getStackTraceString(e));
        }
    }

    /**
     * 旋转偏移已更新的通知。渲染器保持 0° 旋转（应用自行处理预览旋转），
     * rotation_offset 仅在 captureFrameForYuv 中应用于 YUV 截帧/JPEG。
     */
    void updateRotation(int degrees) {
        LogUtil.log("【CS】旋转偏移已更新: " + degrees + "°（渲染器保持0°，仅影响截帧）");
    }

    /** Release all GL renderers. */
    void releaseAllRenderers() {
        GLVideoRenderer.releaseSafely(c2_reader_renderer);
        c2_reader_renderer = null;
        GLVideoRenderer.releaseSafely(c2_reader_renderer_1);
        c2_reader_renderer_1 = null;
        GLVideoRenderer.releaseSafely(c2_renderer);
        c2_renderer = null;
        GLVideoRenderer.releaseSafely(c2_renderer_1);
        c2_renderer_1 = null;
        GLVideoRenderer.releaseSafely(c1_renderer_holder);
        c1_renderer_holder = null;
        GLVideoRenderer.releaseSafely(c1_renderer_texture);
        c1_renderer_texture = null;
    }

    /** Release Camera1 players and renderers (called from stopPreview/release). */
    void releaseCamera1Resources() {
        releaseStreamBackend();
        GLVideoRenderer.releaseSafely(c1_renderer_holder);
        c1_renderer_holder = null;
        GLVideoRenderer.releaseSafely(c1_renderer_texture);
        c1_renderer_texture = null;
        stopAndRelease(mplayer1);
        mplayer1 = null;
        stopAndRelease(mMediaPlayer);
        mMediaPlayer = null;
    }

    // =====================================================================
    // Camera1 stream playback (CQ patch: network stream for Camera1 apps)
    // =====================================================================

    /**
     * Start the shared stream backend for a Camera1 app, rendering into the
     * given preview surface (holder or texture). Returns false when stream
     * mode is off or the source/surface is unusable, letting callers fall
     * back to the local MediaPlayer paths.
     */
    boolean prepareCamera1Stream(Surface holderSurface, Surface textureSurface, boolean playSound) {
        MediaSourceDescriptor source = getMediaSource();
        if (source == null || !source.isStream() || !source.isValid()) {
            return false;
        }
        Surface target = (holderSurface != null && holderSurface.isValid())
                ? holderSurface
                : ((textureSurface != null && textureSurface.isValid()) ? textureSurface : null);
        if (target == null) {
            LogUtil.log("【CS】Camera1 流模式：无可用 Surface");
            return false;
        }
        releaseStreamBackend();
        GLVideoRenderer.releaseSafely(c1_renderer_holder);
        c1_renderer_holder = null;
        GLVideoRenderer.releaseSafely(c1_renderer_texture);
        c1_renderer_texture = null;
        GLVideoRenderer renderer = GLVideoRenderer.createSafely(target, "c1_stream");
        c1_renderer_holder = renderer;
        try {
            streamBackend = createStreamBackend();
            Surface backendSurface = (renderer != null && renderer.isInitialized())
                    ? renderer.getInputSurface()
                    : target;
            streamBackend.setOutputSurface(backendSurface);
            streamBackend.setVolume(playSound ? 1.0f : 0f);
            streamBackend.setLooping(false); // streams don't loop
            streamBackend.setListener(new SurfacePlayerBackend.Listener() {
                @Override
                public void onReady() {
                    LogUtil.log("【CS】Camera1 流播放器就绪");
                }

                @Override
                public void onError(String message, Throwable cause) {
                    LogUtil.log("【CS】Camera1 流播放器错误: " + message
                            + (cause != null ? " " + cause : ""));
                }

                @Override
                public void onDisconnected() {
                    LogUtil.log("【CS】Camera1 流断开连接");
                }

                @Override
                public void onReconnected() {
                    LogUtil.log("【CS】Camera1 流重连成功");
                }

                @Override
                public void onCompletion() {
                    LogUtil.log("【CS】Camera1 流播放完成");
                }
            });
            streamBackend.open(source);
            LogUtil.log("【CS】Camera1 流模式启动: " + source.streamUrl);
            return true;
        } catch (Exception e) {
            LogUtil.log("【CS】Camera1 流模式初始化失败: " + android.util.Log.getStackTraceString(e));
            releaseStreamBackend();
            return false;
        }
    }

    /** Release Camera2 players and renderers (called from onOpened). */
    void releaseCamera2Resources() {
        releaseStreamBackend();
        GLVideoRenderer.releaseSafely(c2_renderer);
        c2_renderer = null;
        GLVideoRenderer.releaseSafely(c2_renderer_1);
        c2_renderer_1 = null;
        GLVideoRenderer.releaseSafely(c2_reader_renderer);
        c2_reader_renderer = null;
        GLVideoRenderer.releaseSafely(c2_reader_renderer_1);
        c2_reader_renderer_1 = null;
        stopAndRelease(c2_player);
        c2_player = null;
        stopAndRelease(c2_reader_player_1);
        c2_reader_player_1 = null;
        stopAndRelease(c2_reader_player);
        c2_reader_player = null;
        stopAndRelease(c2_player_1);
        c2_player_1 = null;
        lastC2ReaderSurface = null;
        lastC2ReaderSurface1 = null;
        lastC2PreviewSurface = null;
        lastC2PreviewSurface1 = null;
    }

    private void stopAndRelease(MediaPlayer player) {
        if (player == null)
            return;
        closePfd(player);
        try {
            player.stop();
        } catch (Exception ignored) {
        }
        player.release();
    }

    // =====================================================================
    // Private: three-tier surface rendering setup (local mode)
    // =====================================================================

    private void setupMediaPlayer(MediaPlayer player, GLVideoRenderer[] rendererRef,
            SurfaceRelay[] relayRef, Surface targetSurface, String tag, boolean playSound) {
        if (targetSurface == null) {
            LogUtil.w("【CS】【播放器】[" + tag + "] 目标 Surface 为 null，跳过初始化");
            return;
        }
        boolean isSurfaceValid = targetSurface.isValid();
        LogUtil.log("【CS】【播放器】[" + tag + "] 开始配置 MediaPlayer -> TargetSurface: "
                + targetSurface + " (isValid=" + isSurfaceValid + ")");
        if (!isSurfaceValid) {
            LogUtil.w("【CS】【播放器】【警告】[" + tag + "] 目标 Surface 已失效 (isValid=false)，可能会导致渲染黑屏或失败！");
        }

        GLVideoRenderer.releaseSafely(rendererRef[0]);
        SurfaceRelay.releaseSafely(relayRef[0]);
        rendererRef[0] = null;
        relayRef[0] = null;

        if (!playSound)
            player.setVolume(0, 0);
        player.setLooping(true);
        try {
            // 预览渲染器旋转固定为 0°：应用（如 WhatsApp、LINE）会对预览自行应用相机传感器旋转，
            // CamSwap 不应再叠加旋转，否则本机画面会被双重旋转。
            // video_rotation_offset 仅在 YUV 截帧时通过 captureFrameForYuv 应用，确保对方画面正确。
            int rotation = VideoManager.getConfig().getInt(ConfigManager.KEY_VIDEO_ROTATION_OFFSET, 0);
            rendererRef[0] = GLVideoRenderer.createSafely(targetSurface, tag);
            if (rendererRef[0] != null) {
                player.setSurface(rendererRef[0].getInputSurface());
                rendererRef[0].setRotation(rotation);
                LogUtil.log("【CS】【GL】[" + tag + "] 成功挂载 GL 渲染器 (旋转:" + rotation + "°, InputSurface: "
                        + rendererRef[0].getInputSurface() + ")");
            } else {
                LogUtil.w("【CS】【Relay】[" + tag + "] GL 渲染器创建失败，尝试回退到 SurfaceTexture 中继");
                relayRef[0] = SurfaceRelay.createSafely(targetSurface, tag);
                if (relayRef[0] != null) {
                    player.setSurface(relayRef[0].getInputSurface());
                    relayRef[0].setRotation(rotation);
                    LogUtil.log("【CS】【Relay】[" + tag + "] 挂载 Relay 渲染器成功 (旋转:" + rotation + "°)");
                } else {
                    player.setSurface(targetSurface);
                    LogUtil.w("【CS】【播放器】[" + tag + "] 回退到直接绑定 TargetSurface（无 GL/Relay 加速）");
                }
            }

            android.os.ParcelFileDescriptor pfd = VideoManager.getVideoPFD();
            String currentPath = getVideoPath();
            if (pfd != null) {
                bindPfd(player, pfd);
                player.setDataSource(pfd.getFileDescriptor());
                LogUtil.log("【CS】【播放器】[" + tag + "] 使用 ContentProvider PFD 数据源: fd="
                        + pfd.getFd() + " (路径=" + currentPath + ")");
            } else {
                closePfd(player);
                player.setDataSource(currentPath);
                LogUtil.log("【CS】【播放器】[" + tag + "] 使用本地直接文件数据源: " + currentPath);
            }

            player.setOnErrorListener((mp, what, extra) -> {
                String diagnosis = LogUtil.explainMediaPlayerError(what, extra);
                LogUtil.e("【CS】【播放器】【致命错误】[" + tag + "] MediaPlayer 触发错误 -> "
                        + diagnosis + " (what=" + what + ", extra=" + extra + ")", null);
                return true;
            });

            player.setOnInfoListener((mp, what, extra) -> {
                if (what == android.media.MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START) {
                    LogUtil.log("【CS】【播放器】[" + tag + "] 接收到首帧渲染就绪信号 (MEDIA_INFO_VIDEO_RENDERING_START)！画面已成功上屏！");
                } else if (what == android.media.MediaPlayer.MEDIA_INFO_BUFFERING_START) {
                    LogUtil.log("【CS】【播放器】[" + tag + "] 正在缓冲数据...");
                } else if (what == android.media.MediaPlayer.MEDIA_INFO_BUFFERING_END) {
                    LogUtil.log("【CS】【播放器】[" + tag + "] 数据缓冲完成");
                } else {
                    LogUtil.log("【CS】【播放器】[" + tag + "] MediaPlayer info: what=" + what + " extra=" + extra);
                }
                return false;
            });

            long prepareStart = SystemClock.elapsedRealtime();
            player.prepare();
            long prepareDuration = SystemClock.elapsedRealtime() - prepareStart;

            int videoW = player.getVideoWidth();
            int videoH = player.getVideoHeight();
            int duration = player.getDuration();

            player.start();
            markCamera2PlaybackStarted(player, tag);
            LogUtil.log("【CS】【播放器】[" + tag + "] 播放器启动成功！(视频规格: " + videoW + "x" + videoH
                    + ", 时长: " + duration + "ms, prepare耗时: " + prepareDuration + "ms, isPlaying=" + player.isPlaying() + ")");
        } catch (Exception e) {
            LogUtil.e("【CS】【播放器】【初始化异常】[" + tag + "] 启动播放器失败: " + e.getMessage(), e);
        }
    }
}
