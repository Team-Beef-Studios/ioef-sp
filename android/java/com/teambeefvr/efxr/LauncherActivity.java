package com.teambeefvr.efxr;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Storage-permission gate.  Native engine init happens in GLES3JNIActivity, so
 * we must secure all-files access (MANAGE_EXTERNAL_STORAGE on Android 11+, the
 * legacy runtime permission below) and deploy bundled assets to /sdcard/EFXR
 * BEFORE that activity starts.  Modeled on OpenJKDF2's LauncherActivity.
 */
public class LauncherActivity extends Activity {

	private static final String TAG = "EFXR";
	private static final int REQUEST_MANAGE_ALL_FILES = 2296;
	private static final int REQUEST_STORAGE_PERMISSION = 2297;
	private static final String GAME_FOLDER = "/sdcard/EFXR";
	private static final String BASE_FOLDER = GAME_FOLDER + "/baseEF";

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		Log.v(TAG, "LauncherActivity::onCreate()");
		super.onCreate(savedInstanceState);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
		checkPermissionAndLaunch();
	}

	private void checkPermissionAndLaunch() {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
			if (!Environment.isExternalStorageManager()) {
				Log.v(TAG, "Requesting MANAGE_EXTERNAL_STORAGE (Android 11+)...");
				Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
				intent.setData(Uri.fromParts("package", getPackageName(), null));
				startActivityForResult(intent, REQUEST_MANAGE_ALL_FILES);
			} else {
				launchVRActivity();
			}
		} else {
			if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
				requestPermissions(new String[]{
						Manifest.permission.READ_EXTERNAL_STORAGE,
						Manifest.permission.WRITE_EXTERNAL_STORAGE
				}, REQUEST_STORAGE_PERMISSION);
			} else {
				launchVRActivity();
			}
		}
	}

	private void launchVRActivity() {
		copyAssetsIfNeeded();
		Intent intent = new Intent(this, GLES3JNIActivity.class);
		intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
		startActivity(intent);
		finish();
	}

	private void copyAssetsIfNeeded() {
		new File(BASE_FOLDER + "/save").mkdirs();

		// commandline.txt: only seed it if the user hasn't supplied one.
		copyAssetFile("commandline.txt", GAME_FOLDER + "/commandline.txt", false);
		copyAssetFile("hmconfig.cfg", GAME_FOLDER + "/baseEF/hmconfig.cfg", false);
		// VR overrides pk3: always refresh (force) so updates land.
		//copyAssetFile("vr_assets_base.pk3", BASE_FOLDER + "/z_vr_assets_base.pk3", true);
		// NOTE: retail pak0.pk3 / pak3.pk3 are user-sideloaded into baseEF (licensing).
	}

	private void copyAssetFile(String assetName, String destPath, boolean force) {
		File dest = new File(destPath);
		if (dest.exists() && !force) {
			return;
		}
		File parent = dest.getParentFile();
		if (parent != null && !parent.exists()) {
			parent.mkdirs();
		}
		AssetManager assets = getAssets();
		try (InputStream in = assets.open(assetName);
		     OutputStream out = new FileOutputStream(destPath)) {
			byte[] buf = new byte[4096];
			int n;
			while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
			Log.v(TAG, "Copied asset " + assetName + " -> " + destPath);
		} catch (Exception e) {
			Log.w(TAG, "Asset copy skipped for " + assetName + ": " + e);
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == REQUEST_MANAGE_ALL_FILES && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
			if (Environment.isExternalStorageManager()) {
				launchVRActivity();
			} else {
				Log.w(TAG, "Storage permission not granted, exiting.");
				finishAffinity();
			}
		}
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		if (requestCode == REQUEST_STORAGE_PERMISSION) {
			if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
				launchVRActivity();
			} else {
				Log.w(TAG, "Storage permission denied, exiting.");
				finishAffinity();
			}
		}
	}
}
