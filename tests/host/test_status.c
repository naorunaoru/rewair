#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rewair_json.h"
#include "rewair_state.h"

int main( void )
{
    static rewair_status_t st;
    static char buf[1024];
    int len;

    memset( &st, 0, sizeof( st ) );
    strcpy( st.name, "Living room" );
    strcpy( st.fw, "rewair 0.4.0-dev" );
    st.sens_valid = 1u;
    st.sens.temp = 2240; st.sens.humid = 4600; st.sens.co2 = 64000;
    st.sens.voc = 18000; st.sens.dust = 600; st.sens.light = 320;
    st.score = 92u; strcpy( st.score_color, "green" );
    st.idx_co2 = 1u;
    st.wifi_mode = 0u;
    strcpy( st.ssid, "mikrotik-2g" ); st.rssi = -54;
    strcpy( st.ip, "192.168.88.10" ); strcpy( st.gw, "192.168.88.1" );
    strcpy( st.dns, "192.168.88.1" ); strcpy( st.mac, "aa:bb:cc:dd:ee:ff" );
    st.connected_s = 11520u; st.drops = 0u; st.saved_count = 3u;
    st.time_valid = 1u; st.time_synced = 1u; st.epoch = 1749300000u;
    st.units = 0u; st.time_mode = 0u; st.disp_mode = 0u;
    st.tz_offset_min = 60; st.tz_dst = 1u;
    strcpy( st.tz_zone, "Europe/Lisbon" );
    strcpy( st.tz_posix, "WET0WEST,M3.5.0/1,M10.5.0" );

    len = rewair_json_status( &st, buf, sizeof( buf ) );
    assert( len > 0 );
    assert( strstr( buf, "\"name\":\"Living room\"" ) != NULL );
    assert( strstr( buf, "\"value\":92" ) != NULL );
    assert( strstr( buf, "\"color\":\"green\"" ) != NULL );
    assert( strstr( buf, "\"co2\":1" ) != NULL );          /* index */
    assert( strstr( buf, "\"temp\":22.40" ) != NULL );     /* centi -> decimal */
    assert( strstr( buf, "\"co2\":640.00" ) != NULL );     /* sens value */
    assert( strstr( buf, "\"light\":320" ) != NULL );
    assert( strstr( buf, "\"mode\":\"sta\"" ) != NULL );
    assert( strstr( buf, "\"rssi\":-54" ) != NULL );
    assert( strstr( buf, "\"synced\":true" ) != NULL );
    assert( strstr( buf, "\"tz_offset\":60" ) != NULL );
    assert( strstr( buf, "\"tz_posix\":\"WET0WEST,M3.5.0\\/1,M10.5.0\"" ) == NULL ); /* no needless escaping */
    assert( strstr( buf, "\"tz_posix\":\"WET0WEST,M3.5.0/1,M10.5.0\"" ) != NULL );
    assert( strstr( buf, "\"units\":\"c\"" ) != NULL );
    assert( strstr( buf, "\"disp_mode\":\"score\"" ) != NULL );

    /* AP-mode variant */
    st.wifi_mode = 1u;
    strcpy( st.ap_ssid, "rewair-setup-1be0" );
    strcpy( st.ap_ip, "192.168.0.1" );
    len = rewair_json_status( &st, buf, sizeof( buf ) );
    assert( len > 0 );
    assert( strstr( buf, "\"mode\":\"ap\"" ) != NULL );
    assert( strstr( buf, "\"ap_ssid\":\"rewair-setup-1be0\"" ) != NULL );

    /* name with a quote must be escaped */
    strcpy( st.name, "Bob's \"lab\"" );
    len = rewair_json_status( &st, buf, sizeof( buf ) );
    assert( len > 0 );
    assert( strstr( buf, "\"name\":\"Bob's \\\"lab\\\"\"" ) != NULL );

    /* truncation reports failure */
    assert( rewair_json_status( &st, buf, 64u ) == -1 );

    /* rewair_json_escape_string: quote and backslash escaping */
    {
        char esc[16];
        uint32_t n = rewair_json_escape_string( "a\"b\\", esc, sizeof( esc ) );
        assert( n == 6u );
        assert( strcmp( esc, "a\\\"b\\\\" ) == 0 );
    }

    /* epoch must serialize unsigned: a post-2038 value must not wrap negative */
    st.epoch = 2400000000u; /* year 2046 */
    len = rewair_json_status( &st, buf, sizeof( buf ) );
    assert( len > 0 );
    assert( strstr( buf, "\"epoch\":2400000000" ) != NULL );

    printf( "test_status OK\n" );
    return 0;
}
