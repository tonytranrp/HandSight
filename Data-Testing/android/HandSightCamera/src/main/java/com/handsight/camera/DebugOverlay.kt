package com.handsight.camera

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.TextView
import android.widget.RelativeLayout
import android.graphics.Color
import java.io.File

class DebugOverlay : Activity() {
    private lateinit var debugText: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val updateRunnable = object : Runnable {
        override fun run() {
            updateDebugInfo()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        debugText = TextView(this).apply {
            setTextColor(Color.GREEN)
            setBackgroundColor(Color.argb(200, 0, 0, 0))
            textSize = 10f
            setPadding(16, 16, 16, 16)
        }

        val layout = RelativeLayout(this).apply {
            addView(debugText, RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                addRule(RelativeLayout.ALIGN_PARENT_TOP)
            })
        }

        setContentView(layout)
    }

    override fun onResume() {
        super.onResume()
        handler.post(updateRunnable)
    }

    override fun onPause() {
        handler.removeCallbacks(updateRunnable)
        super.onPause()
    }

    private fun updateDebugInfo() {
        try {
            val logFile = "/data/local/tmp/handsight_debug.txt"
            if (File(logFile).exists()) {
                val content = File(logFile).readText()
                debugText.text = content
            } else {
                debugText.text = "📡 HandSight Debug Overlay\n\n" +
                        "Waiting for stats...\n" +
                        "Launch camera app to begin.\n\n" +
                        "Metrics will appear here:\n" +
                        "• FPS\n" +
                        "• Frames sent/captured\n" +
                        "• Throughput (MB/s)\n" +
                        "• Connection status"
            }
        } catch (e: Exception) {
            debugText.text = "Error reading stats: ${e.message}"
        }
    }
}
