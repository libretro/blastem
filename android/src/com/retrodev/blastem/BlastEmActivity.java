package com.retrodev.blastem;
import org.libsdl.app.SDLActivity;
import android.os.Build;
import android.os.Bundle;
import android.view.View;


public class BlastEmActivity extends SDLActivity
{
	@Override
    protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		
		//set immersive mode on devices that support it
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
			View blah = mSurface;
			blah.setSystemUiVisibility(
				View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
			);
		}
	}
}