package org.macnativegames.gtavandroid

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
    private external fun nativePortStatus(): String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(28, 28, 28, 28)
        }
        val title = TextView(this).apply {
            text = "GTA V — Android port development"
            textSize = 23f
            setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
        }
        val status = TextView(this).apply {
            textSize = 16f
            setTextColor(0xFFE0E0E0.toInt())
            gravity = Gravity.CENTER
            setPadding(0, 24, 0, 0)
            text = try {
                nativePortStatus()
            } catch (error: UnsatisfiedLinkError) {
                "Native bridge unavailable: ${error.message}"
            }
        }
        root.setBackgroundColor(0xFF101318.toInt())
        root.addView(title)
        root.addView(status)
        val scroll = ScrollView(this).apply { addView(root) }
        setContentView(scroll)
    }

    companion object {
        init {
            System.loadLibrary("gtav_android")
        }
    }
}
