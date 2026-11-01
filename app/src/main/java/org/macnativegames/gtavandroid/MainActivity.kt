package org.macnativegames.gtavandroid

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {
    private external fun nativePortStatus(): String
    private external fun nativeSetSurface(surface: Surface?)

    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20, 20, 20, 20)
            setBackgroundColor(0xFF101318.toInt())
        }
        val title = TextView(this).apply {
            text = "GTA V — Android port development"
            textSize = 22f
            gravity = Gravity.CENTER
            setTextColor(0xFFFFFFFF.toInt())
        }
        status = TextView(this).apply {
            textSize = 14f
            setTextColor(0xFFE0E0E0.toInt())
            setPadding(0, 16, 0, 16)
            text = readNativeStatus()
        }
        val surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
            holder.setFormat(android.graphics.PixelFormat.RGBA_8888)
        }
        root.addView(title, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(status, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(surfaceView, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(root)
    }

    private fun readNativeStatus(): String = try {
        nativePortStatus()
    } catch (error: UnsatisfiedLinkError) {
        "Native bridge unavailable: ${error.message}"
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        nativeSetSurface(holder.surface)
        status.text = readNativeStatus()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        nativeSetSurface(holder.surface)
        status.text = readNativeStatus()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        nativeSetSurface(null)
        status.text = "Android native window: disconnected."
    }

    override fun onDestroy() {
        nativeSetSurface(null)
        super.onDestroy()
    }

    companion object {
        init { System.loadLibrary("gtav_android") }
    }
}
