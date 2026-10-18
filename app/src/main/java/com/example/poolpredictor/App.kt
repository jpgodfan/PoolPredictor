package com.example.poolpredictor

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.PointF
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.Image
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.provider.Settings
import android.util.AttributeSet
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationCompat
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

class MainActivity : AppCompatActivity() {

    private lateinit var projectionManager: MediaProjectionManager

    private var pendingProjection: Pair<Int, Intent>? = null

    private val requestProjection =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                pendingProjection = result.resultCode to result.data!!

                if (Settings.canDrawOverlays(this)) {
                    startPredictorService(result.resultCode, result.data!!)
                } else {
                    requestOverlayPermission.launch(
                        Intent(
                            Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                            Uri.parse("package:$packageName")
                        )
                    )
                }
            } else {
                Toast.makeText(this, "Screen capture permission denied", Toast.LENGTH_SHORT).show()
            }
        }

    private val requestOverlayPermission =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            val pending = pendingProjection ?: return@registerForActivityResult

            if (Settings.canDrawOverlays(this)) {
                startPredictorService(pending.first, pending.second)
            } else {
                Toast.makeText(this, "Overlay permission is required", Toast.LENGTH_SHORT).show()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        projectionManager = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager

        findViewById<Button>(R.id.startButton).setOnClickListener {
            requestProjection.launch(projectionManager.createScreenCaptureIntent())
        }

        findViewById<Button>(R.id.stopButton).setOnClickListener {
            stopService(Intent(this, PredictorService::class.java))
            Toast.makeText(this, "Overlay stopped", Toast.LENGTH_SHORT).show()
        }
    }

    private fun startPredictorService(resultCode: Int, data: Intent) {
        val intent = Intent(this, PredictorService::class.java)
            .putExtra("resultCode", resultCode)
            .putExtra("projectionData", data)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }

        Toast.makeText(this, "Overlay started", Toast.LENGTH_SHORT).show()
    }
}

class PredictorService : Service() {

    companion object {
        private const val CHANNEL_ID = "pool_predictor"
        private const val NOTIFICATION_ID = 1001
    }

    private lateinit var windowManager: WindowManager
    private lateinit var overlayView: OverlayView

    private lateinit var projectionManager: MediaProjectionManager

    private var mediaProjection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null

    private val captureThread = HandlerThread("capture-thread").apply { start() }
    private val processThread = HandlerThread("process-thread").apply { start() }

    private val captureHandler = Handler(captureThread.looper)
    private val processHandler = Handler(processThread.looper)

    private val nativePredictor = NativePredictor()
    private val processing = AtomicBoolean(false)

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()

        windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        projectionManager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager

        overlayView = OverlayView(this)
        addOverlayView()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundWithProjectionType()

        val resultCode = intent?.getIntExtra("resultCode", Activity.RESULT_CANCELED)
            ?: Activity.RESULT_CANCELED

        val projectionData = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent?.getParcelableExtra("projectionData", Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent?.getParcelableExtra("projectionData")
        }

        stopCapture()

        if (resultCode == Activity.RESULT_OK && projectionData != null) {
            startScreenCapture(resultCode, projectionData)
        } else {
            stopSelf()
        }

        return START_NOT_STICKY
    }

    private fun startForegroundWithProjectionType() {
        createNotificationChannel()

        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        val notification: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Pool Predictor")
            .setContentText("Predicting billiards paths")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pendingIntent)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Pool Predictor Service",
                NotificationManager.IMPORTANCE_LOW
            )

            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun addOverlayView() {
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE
        }

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    or WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    or WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
            PixelFormat.TRANSLUCENT
        )

        params.gravity = Gravity.TOP or Gravity.START

        windowManager.addView(overlayView, params)
    }

    private fun startScreenCapture(resultCode: Int, data: Intent) {
        val metrics = resources.displayMetrics

        val width = metrics.widthPixels
        val height = metrics.heightPixels
        val density = metrics.densityDpi

        imageReader = ImageReader.newInstance(
            width,
            height,
            PixelFormat.RGBA_8888,
            2
        )

        mediaProjection = projectionManager.getMediaProjection(resultCode, data)

        mediaProjection?.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                stopSelf()
            }
        }, captureHandler)

        virtualDisplay = mediaProjection?.createVirtualDisplay(
            "PoolPredictorDisplay",
            width,
            height,
            density,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            imageReader!!.surface,
            null,
            captureHandler
        )

        imageReader?.setOnImageAvailableListener(
            { reader ->
                onFrameAvailable(reader)
            },
            captureHandler
        )
    }

    private fun onFrameAvailable(reader: ImageReader) {
        val image = reader.acquireLatestImage() ?: return

        if (processing.getAndSet(true)) {
            image.close()
            return
        }

        processHandler.post {
            try {
                val frame = FrameConverter.imageToRgbaBytes(image)

                val json = nativePredictor.predictJson(
                    frame.bytes,
                    frame.width,
                    frame.height
                )

                val prediction = PredictionParser.parse(json)

                overlayView.post {
                    overlayView.updatePrediction(prediction)
                }
            } catch (e: Exception) {
                e.printStackTrace()
            } finally {
                image.close()
                processing.set(false)
            }
        }
    }

    private fun stopCapture() {
        virtualDisplay?.release()
        virtualDisplay = null

        imageReader?.close()
        imageReader = null

        mediaProjection?.stop()
        mediaProjection = null
    }

    override fun onDestroy() {
        stopCapture()

        captureThread.quitSafely()
        processThread.quitSafely()

        try {
            windowManager.removeView(overlayView)
        } catch (_: Exception) {
        }

        super.onDestroy()
    }
}

class OverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private var prediction: Prediction? = null

    private val boundaryPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
        strokeWidth = 2f
        alpha = 76
    }

    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        alpha = 46
    }

    private val pathPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        strokeWidth = 5f
        alpha = 158
    }

    private val ghostPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
        strokeWidth = 2f
        alpha = 128
        pathEffect = DashPathEffect(floatArrayOf(12f, 10f), 0f)
    }

    private val pocketPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 5f
        alpha = 230
    }

    fun updatePrediction(newPrediction: Prediction?) {
        prediction = newPrediction
        postInvalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val pred = prediction ?: return
        if (pred.screenWidth <= 0 || pred.screenHeight <= 0) return

        val scaleX = width.toFloat() / pred.screenWidth
        val scaleY = height.toFloat() / pred.screenHeight

        drawBoundary(canvas, pred, scaleX, scaleY)
        drawPaths(canvas, pred, scaleX, scaleY)
        drawGhosts(canvas, pred, scaleX, scaleY)
        drawPocketMarks(canvas, pred, scaleX, scaleY)
    }

    private fun drawBoundary(
        canvas: Canvas,
        pred: Prediction,
        scaleX: Float,
        scaleY: Float
    ) {
        val l = pred.roiRect.left * scaleX
        val t = pred.roiRect.top * scaleY
        val r = pred.roiRect.right * scaleX
        val b = pred.roiRect.bottom * scaleY

        canvas.drawRect(l, t, r, b, boundaryPaint)
    }

    private fun drawPaths(
        canvas: Canvas,
        pred: Prediction,
        scaleX: Float,
        scaleY: Float
    ) {
        for (ballPath in pred.paths) {
            if (ballPath.points.size < 2) continue

            val path = Path()

            val first = ballPath.points[0]
            path.moveTo(first.x * scaleX, first.y * scaleY)

            for (i in 1 until ballPath.points.size) {
                val p = ballPath.points[i]
                path.lineTo(p.x * scaleX, p.y * scaleY)
            }

            outlinePaint.color = ballPath.color
            outlinePaint.strokeWidth = max(4f, ballPath.radius * 2f * scaleX)
            canvas.drawPath(path, outlinePaint)

            pathPaint.color = ballPath.color
            pathPaint.strokeWidth = max(3f, 2f * scaleX)
            canvas.drawPath(path, pathPaint)
        }
    }

    private fun drawGhosts(
        canvas: Canvas,
        pred: Prediction,
        scaleX: Float,
        scaleY: Float
    ) {
        val radius = pred.ballRadius * scaleX

        for (ghost in pred.ghostPoints) {
            canvas.drawCircle(
                ghost.x * scaleX,
                ghost.y * scaleY,
                radius,
                ghostPaint
            )
        }
    }

    private fun drawPocketMarks(
        canvas: Canvas,
        pred: Prediction,
        scaleX: Float,
        scaleY: Float
    ) {
        val radius = pred.ballRadius * 1.15f * scaleX

        for (mark in pred.pocketMarks) {
            pocketPaint.color = mark.color

            canvas.drawCircle(
                mark.x * scaleX,
                mark.y * scaleY,
                radius,
                pocketPaint
            )
        }
    }
}

data class RgbaFrame(
    val bytes: ByteArray,
    val width: Int,
    val height: Int
)

object FrameConverter {

    fun imageToRgbaBytes(image: Image): RgbaFrame {
        require(image.format == PixelFormat.RGBA_8888) {
            "Expected RGBA_8888 image"
        }

        val width = image.width
        val height = image.height

        val plane = image.planes[0]
        val buffer = plane.buffer

        val rowStride = plane.rowStride
        val pixelStride = plane.pixelStride
        val rowBytes = width * 4

        val bytes = ByteArray(rowBytes * height)

        if (pixelStride == 4) {
            for (y in 0 until height) {
                buffer.position(y * rowStride)
                buffer.get(bytes, y * rowBytes, rowBytes)
            }
        } else {
            for (y in 0 until height) {
                val rowBase = y * rowStride
                val destBase = y * rowBytes

                for (x in 0 until width) {
                    val src = rowBase + x * pixelStride
                    val dest = destBase + x * 4

                    bytes[dest] = buffer.get(src)
                    bytes[dest + 1] = buffer.get(src + 1)
                    bytes[dest + 2] = buffer.get(src + 2)
                    bytes[dest + 3] = buffer.get(src + 3)
                }
            }
        }

        return RgbaFrame(
            bytes = bytes,
            width = width,
            height = height
        )
    }
}

class NativePredictor {

    external fun predictJson(
        rgba: ByteArray,
        width: Int,
        height: Int
    ): String

    companion object {
        init {
            try {
                System.loadLibrary("opencv_java4")
            } catch (t: Throwable) {
                t.printStackTrace()
            }

            try {
                System.loadLibrary("poolpredictor")
            } catch (t: Throwable) {
                t.printStackTrace()
            }
        }
    }
}

data class PocketMark(
    val x: Float,
    val y: Float,
    val color: Int
)

data class BallPath(
    val role: String,
    val color: Int,
    val radius: Float,
    val points: List<PointF>
)

data class Prediction(
    val screenWidth: Int,
    val screenHeight: Int,
    val ballRadius: Float,
    val roiRect: Rect,
    val paths: List<BallPath>,
    val ghostPoints: List<PointF>,
    val pocketMarks: List<PocketMark>
) {
    companion object {
        fun empty(width: Int = 0, height: Int = 0): Prediction {
            return Prediction(
                screenWidth = width,
                screenHeight = height,
                ballRadius = 1f,
                roiRect = Rect(0, 0, width, height),
                paths = emptyList(),
                ghostPoints = emptyList(),
                pocketMarks = emptyList()
            )
        }
    }
}

object PredictionParser {

    fun parse(json: String?): Prediction {
        if (json.isNullOrBlank()) return Prediction.empty()

        return try {
            val root = JSONObject(json)

            val screenWidth = root.optInt("screenWidth", 0)
            val screenHeight = root.optInt("screenHeight", 0)
            val ballRadius = root.optDouble("ballRadius", 1.0).toFloat()

            val roi = root.optJSONObject("roi")
            val roiRect = if (roi != null) {
                Rect(
                    roi.optInt("left", 0),
                    roi.optInt("top", 0),
                    roi.optInt("right", 0),
                    roi.optInt("bottom", 0)
                )
            } else {
                Rect(0, 0, screenWidth, screenHeight)
            }

            val paths = mutableListOf<BallPath>()
            val pathsArray = root.optJSONArray("paths")

            if (pathsArray != null) {
                for (i in 0 until pathsArray.length()) {
                    val pathObject = pathsArray.getJSONObject(i)

                    val role = pathObject.optString("role", "object")
                    val color = pathObject.optInt("color", 0)
                    val radius = pathObject.optDouble("radius", 1.0).toFloat()

                    val points = mutableListOf<PointF>()
                    val pointsArray = pathObject.optJSONArray("points")

                    if (pointsArray != null) {
                        for (j in 0 until pointsArray.length()) {
                            val pointArray = pointsArray.getJSONArray(j)

                            val x = pointArray.getDouble(0).toFloat()
                            val y = pointArray.getDouble(1).toFloat()

                            points.add(PointF(x, y))
                        }
                    }

                    paths.add(
                        BallPath(
                            role = role,
                            color = color,
                            radius = radius,
                            points = points
                        )
                    )
                }
            }

            val ghostPoints = mutableListOf<PointF>()
            val ghostArray = root.optJSONArray("ghosts")

            if (ghostArray != null) {
                for (i in 0 until ghostArray.length()) {
                    val p = ghostArray.getJSONArray(i)

                    ghostPoints.add(
                        PointF(
                            p.getDouble(0).toFloat(),
                            p.getDouble(1).toFloat()
                        )
                    )
                }
            }

            val pocketMarks = mutableListOf<PocketMark>()
            val pocketArray = root.optJSONArray("pocketMarks")

            if (pocketArray != null) {
                for (i in 0 until pocketArray.length()) {
                    val p = pocketArray.getJSONObject(i)

                    pocketMarks.add(
                        PocketMark(
                            x = p.optDouble("x", 0.0).toFloat(),
                            y = p.optDouble("y", 0.0).toFloat(),
                            color = p.optInt("color", 0)
                        )
                    )
                }
            }

            Prediction(
                screenWidth = screenWidth,
                screenHeight = screenHeight,
                ballRadius = ballRadius,
                roiRect = roiRect,
                paths = paths,
                ghostPoints = ghostPoints,
                pocketMarks = pocketMarks
            )
        } catch (e: Exception) {
            e.printStackTrace()
            Prediction.empty()
        }
    }
}
