/*
   This file is part of zsrelay a srelay port for the iPhone.

   Copyright (C) 2008 Rene Koecher <shirk@bitspin.org>

   zsrelay is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. 
   */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#import "ZSRelayApp.h"

@implementation ZSRelayApp (Icons)

-(void)showIcon:(NSString*)iconName
{
    [self showIcon:iconName
     makeExclusive:YES];
}

-(void)showIcon:(NSString*)iconName makeExclusive:(BOOL)exclusive
{
    NSMutableString *fullIconName = [iconName mutableCopy];
    if ([self displayStatusIcons] == NO)
      {
	[fullIconName release];
	return;
      }

    if (_connected == NO)
      [fullIconName appendString:@"NOP"];

    if (exclusive == YES)
      [self removeAllIcons];

    if ([self respondsToSelector:@selector(addStatusBarImageNamed:removeOnAbnormalExit:)] == YES)
      [self addStatusBarImageNamed:fullIconName
	removeOnAbnormalExit:YES];
    else
      [self addStatusBarImageNamed:fullIconName
	removeOnExit:NO];

    [fullIconName release];
}

-(void)removeIcon:(NSString*)iconName
{
    NSMutableString *fullIconName = [iconName mutableCopy];

    if ([self displayStatusIcons] == NO)
      {
	[fullIconName release];
	return;
      }

    if (_connected == NO)
      [fullIconName appendString:@"NOP"];

    NSLog(@"DisplayIcon: %@", fullIconName);
    [self removeStatusBarImageNamed:fullIconName];
    [fullIconName release];
}

-(void)removeAllIcons
{
    [self removeStatusBarImageNamed:@"ZSRelay"];
    [self removeStatusBarImageNamed:@"ZSRelayNOP"];
    [self removeStatusBarImageNamed:@"ZSRelayInsomnia"];
    [self removeStatusBarImageNamed:@"ZSRelayInsomniaNOP"];
}
@end

