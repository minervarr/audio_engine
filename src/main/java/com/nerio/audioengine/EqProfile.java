package com.nerio.audioengine;

import android.content.Context;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.GZIPInputStream;

public class EqProfile {

    private static final String TAG = "EqProfile";

    public String name;
    public String source;
    public String form;
    public double preamp;
    public List<Filter> filters;

    public static class Filter {
        public String type; // "PK", "LSC", "HSC"
        public double fc;   // center frequency Hz
        public double gain; // dB
        public double q;
    }

    private static List<EqProfile> cachedProfiles;

    public static synchronized List<EqProfile> loadAll(Context context) {
        if (cachedProfiles != null) return cachedProfiles;

        try {
            InputStream is = context.getAssets().open("eq_profiles.bin");
            GZIPInputStream gis = new GZIPInputStream(is);
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = gis.read(buf)) != -1) {
                bos.write(buf, 0, n);
            }
            gis.close();

            String json = bos.toString("UTF-8");
            JSONArray arr = new JSONArray(json);
            List<EqProfile> profiles = new ArrayList<>(arr.length());

            for (int i = 0; i < arr.length(); i++) {
                JSONObject obj = arr.getJSONObject(i);
                EqProfile p = new EqProfile();
                p.name = obj.getString("name");
                p.source = obj.optString("source", "");
                p.form = obj.optString("form", "");
                p.preamp = obj.getDouble("preamp");

                JSONArray fa = obj.getJSONArray("filters");
                p.filters = new ArrayList<>(fa.length());
                for (int j = 0; j < fa.length(); j++) {
                    JSONObject fo = fa.getJSONObject(j);
                    Filter f = new Filter();
                    f.type = fo.getString("type");
                    f.fc = fo.getDouble("fc");
                    f.gain = fo.getDouble("gain");
                    f.q = fo.getDouble("q");
                    p.filters.add(f);
                }
                profiles.add(p);
            }

            Collections.sort(profiles, (a, b) -> a.name.compareToIgnoreCase(b.name));
            cachedProfiles = profiles;
            Log.i(TAG, "Loaded " + profiles.size() + " EQ profiles");
            return profiles;

        } catch (Exception e) {
            Log.e(TAG, "Failed to load EQ profiles", e);
            cachedProfiles = Collections.emptyList();
            return cachedProfiles;
        }
    }

    /**
     * Find a profile by name + source + form.
     */
    public static EqProfile find(Context context, String name, String source, String form) {
        if (name == null || name.isEmpty()) return null;
        for (EqProfile p : loadAll(context)) {
            if (p.name.equals(name) && p.source.equals(source) && p.form.equals(form)) {
                return p;
            }
        }
        return null;
    }
}
