package com.ravi.gtavnative

import android.app.Activity
import android.os.Bundle
import android.graphics.Typeface
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var status: TextView
    private external fun nativeVulkanInfo(): String
    private external fun nativeInspectSurface(surface: Surface): String
    private external fun nativeRenderFrame(surface: Surface, width: Int, height: Int): String

    companion object {
        init {
            System.loadLibrary("gtav_native")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val title = TextView(this).apply {
            text = "GTA V Native Android"
            textSize = 25f
            typeface = Typeface.DEFAULT_BOLD
        }
        status = TextView(this).apply {
            text = nativeVulkanInfo() + "\n\nWaiting for Android surface…"
            textSize = 14f
            setPadding(0, 20, 0, 20)
        }
        val previewSurface = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
        }
        val note = TextView(this).apply {
            text = "Vulkan surface compatibility test • No game renderer yet"
            textSize = 12f
            setPadding(0, 16, 0, 0)
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(28, 20, 28, 20)
            addView(title)
            addView(status)
            addView(previewSurface, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 180
            ))
            addView(note)
        })
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        status.text = nativeInspectSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        status.text = nativeRenderFrame(holder.surface, width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        status.text = "Android surface paused. Vulkan presentation test will rerun when resumed."
    }
}
