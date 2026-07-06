#include "rewair_tz.h"

#include <string.h>

/* Days-from-civil (Howard Hinnant's algorithm), valid for year >= 1970 */
static uint32_t days_from_civil( uint32_t y, uint32_t m, uint32_t d )
{
    uint32_t era, yoe, doy, doe;

    y -= m <= 2u ? 1u : 0u;
    era = y / 400u;
    yoe = y - era * 400u;
    doy = ( 153u * ( m + ( m > 2u ? (uint32_t)-3 : 9u ) ) + 2u ) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 719468u;   /* days since 1970-01-01 */
}

static uint32_t weekday_of_days( uint32_t days )
{
    return ( days + 4u ) % 7u;   /* 1970-01-01 was Thursday(4); 0 = Sunday */
}

static uint32_t days_in_month( uint32_t year, uint32_t month )
{
    static const uint8_t dim[12] = { 31u,28u,31u,30u,31u,30u,31u,31u,30u,31u,30u,31u };
    if ( month == 2u && ( ( year % 4u == 0u && year % 100u != 0u ) || year % 400u == 0u ) )
    {
        return 29u;
    }
    return dim[month - 1u];
}

uint32_t rewair_tz_rule_day( uint32_t year, uint8_t month, uint8_t week, uint8_t dow )
{
    uint32_t first_days = days_from_civil( year, month, 1u );
    uint32_t first_dow  = weekday_of_days( first_days );
    uint32_t day = 1u + ( ( 7u + (uint32_t)dow - first_dow ) % 7u ) + ( (uint32_t)week - 1u ) * 7u;
    uint32_t dim = days_in_month( year, month );

    while ( day > dim )
    {
        day -= 7u;
    }
    return day;
}

/* Epoch (UTC) of an M-form transition in a given year, given the local offset
 * in effect just before the transition. */
static uint32_t rule_to_utc_epoch( uint32_t year, uint8_t month, uint8_t week,
                                   uint8_t dow, uint32_t local_time_s,
                                   int16_t active_offset_min )
{
    uint32_t day = rewair_tz_rule_day( year, month, week, dow );

    return days_from_civil( year, month, day ) * 86400u + local_time_s -
           (uint32_t)( (int32_t)active_offset_min * 60 );
}

static const char* skip_name( const char* p )
{
    if ( *p == '<' )
    {
        while ( *p != '\0' && *p != '>' )
        {
            p++;
        }
        return *p == '>' ? p + 1 : p;
    }
    while ( ( *p >= 'A' && *p <= 'Z' ) || ( *p >= 'a' && *p <= 'z' ) )
    {
        p++;
    }
    return p;
}

/* Parses [+|-]h[h][:mm] POSIX offset -> ISO minutes east (sign inverted).
 * Returns updated pointer, or NULL on parse failure. */
static const char* parse_offset( const char* p, int16_t* out_min )
{
    int32_t sign = 1;
    int32_t hours = 0;
    int32_t mins = 0;
    int digits = 0;

    if ( *p == '-' )
    {
        sign = -1;
        p++;
    }
    else if ( *p == '+' )
    {
        p++;
    }
    while ( *p >= '0' && *p <= '9' && digits < 3 )
    {
        hours = hours * 10 + ( *p - '0' );
        p++;
        digits++;
    }
    if ( digits == 0 )
    {
        return NULL;
    }
    if ( *p == ':' )
    {
        p++;
        digits = 0;
        while ( *p >= '0' && *p <= '9' && digits < 2 )
        {
            mins = mins * 10 + ( *p - '0' );
            p++;
            digits++;
        }
    }
    *out_min = (int16_t)( -sign * ( hours * 60 + mins ) );
    return p;
}

/* Parses ",Mm.w.d[/time]" -> rule fields. Returns updated pointer or NULL. */
static const char* parse_mrule( const char* p, uint8_t* month, uint8_t* week,
                                uint8_t* dow, uint32_t* time_s )
{
    uint32_t v[3] = { 0u, 0u, 0u };
    uint32_t i;

    if ( *p != ',' || *( p + 1 ) != 'M' )
    {
        return NULL;
    }
    p += 2;
    for ( i = 0u; i < 3u; i++ )
    {
        uint32_t n = 0u;
        int digits = 0;
        while ( *p >= '0' && *p <= '9' )
        {
            n = n * 10u + (uint32_t)( *p - '0' );
            p++;
            digits++;
        }
        if ( digits == 0 )
        {
            return NULL;
        }
        v[i] = n;
        if ( i < 2u )
        {
            if ( *p != '.' )
            {
                return NULL;
            }
            p++;
        }
    }
    if ( v[0] < 1u || v[0] > 12u || v[1] < 1u || v[1] > 5u || v[2] > 6u )
    {
        return NULL;
    }
    *month = (uint8_t)v[0];
    *week  = (uint8_t)v[1];
    *dow   = (uint8_t)v[2];
    *time_s = 2u * 3600u;
    if ( *p == '/' )
    {
        uint32_t h = 0u;
        uint32_t m = 0u;
        p++;
        while ( *p >= '0' && *p <= '9' )
        {
            h = h * 10u + (uint32_t)( *p - '0' );
            p++;
        }
        if ( *p == ':' )
        {
            p++;
            while ( *p >= '0' && *p <= '9' )
            {
                m = m * 10u + (uint32_t)( *p - '0' );
                p++;
            }
        }
        *time_s = h * 3600u + m * 60u;
    }
    return p;
}

int rewair_tz_parse( const char* tz, rewair_tz_rule_t* out )
{
    const char* p = tz;
    const char* after_name;

    memset( out, 0, sizeof( *out ) );
    if ( tz == NULL || tz[0] == '\0' )
    {
        return -1;
    }

    after_name = skip_name( p );
    if ( after_name == p )
    {
        return -1;
    }
    p = parse_offset( after_name, &out->std_offset_min );
    if ( p == NULL )
    {
        return -1;
    }

    after_name = skip_name( p );
    if ( after_name == p )
    {
        return *p == '\0' ? 0 : -1;   /* no DST name: must be end of string */
    }
    p = after_name;
    out->has_dst = 1u;
    out->dst_offset_min = (int16_t)( out->std_offset_min + 60 );
    if ( *p != ',' && *p != '\0' )
    {
        const char* q = parse_offset( p, &out->dst_offset_min );
        if ( q == NULL )
        {
            return -1;
        }
        p = q;
    }

    p = parse_mrule( p, &out->start_month, &out->start_week, &out->start_dow, &out->start_time_s );
    if ( p == NULL )
    {
        return -1;
    }
    p = parse_mrule( p, &out->end_month, &out->end_week, &out->end_dow, &out->end_time_s );
    if ( p == NULL )
    {
        return -1;
    }
    return 0;
}

void rewair_tz_eval( const rewair_tz_rule_t* rule, uint32_t utc_epoch,
                     int16_t* offset_min_out, uint8_t* dst_out )
{
    uint32_t year;
    uint32_t start_utc;
    uint32_t end_utc;
    uint8_t in_dst;

    if ( rule->has_dst == 0u )
    {
        *offset_min_out = rule->std_offset_min;
        *dst_out = 0u;
        return;
    }

    /* derive the year from the epoch (UTC) */
    {
        uint32_t days = utc_epoch / 86400u;
        uint32_t z = days + 719468u;
        uint32_t era = z / 146097u;
        uint32_t doe = z - era * 146097u;
        uint32_t yoe = ( doe - doe / 1460u + doe / 36524u - doe / 146096u ) / 365u;
        uint32_t doy = doe - ( 365u * yoe + yoe / 4u - yoe / 100u );
        uint32_t mp = ( 5u * doy + 2u ) / 153u;
        uint32_t m = mp + ( mp < 10u ? 3u : (uint32_t)-9 );
        year = yoe + era * 400u + ( m <= 2u ? 1u : 0u );
    }

    start_utc = rule_to_utc_epoch( year, rule->start_month, rule->start_week,
                                   rule->start_dow, rule->start_time_s, rule->std_offset_min );
    end_utc   = rule_to_utc_epoch( year, rule->end_month, rule->end_week,
                                   rule->end_dow, rule->end_time_s, rule->dst_offset_min );

    if ( start_utc <= end_utc )
    {
        in_dst = ( utc_epoch >= start_utc && utc_epoch < end_utc ) ? 1u : 0u;
    }
    else
    {
        /* southern hemisphere: DST spans the new year */
        in_dst = ( utc_epoch >= start_utc || utc_epoch < end_utc ) ? 1u : 0u;
    }

    *offset_min_out = in_dst != 0u ? rule->dst_offset_min : rule->std_offset_min;
    *dst_out = in_dst;
}
