package com.ravi.gtavnative

import android.app.Activity
import android.os.Bundle
import android.graphics.Typeface
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    private external fun nativeVulkanInfo(): String

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
        val status = TextView(this).apply {
            text = nativeVulkanInfo()
            textSize = 16f
            setPadding(0, 24, 0, 0)
        }
        val note = TextView(this).apply {
            text = "Developer prototype • Vulkan device probe only"
            textSize = 13f
            setPadding(0, 24, 0, 0)
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(32, 24, 32, 24)
            addView(title)
            addView(status)
            addView(note)
        })
    }
}
